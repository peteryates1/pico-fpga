// Pin connectivity test for JP1 header <-> Pico GPIO 0-15
//
// UART protocol (115200 8N1 on DB_FPGA CP2102N):
//   'R'       -> reads all 16 JP1 pins, sends "XXXX\n" (hex, bit0=JP1[0])
//   'Oxxxx'   -> set output enable mask (hex, bit0=JP1[0])
//   'Dxxxx'   -> set output data (hex)
//   'I'       -> all pins back to input (hi-Z)
//   'W'       -> walk mode: drive each pin HIGH one at a time, 100ms each,
//                sending pin number and readback for each step

module pin_test(
    input  wire        clk_in,      // 50 MHz
    output wire        ser_txd,     // UART TX -> CP2102N
    input  wire        ser_rxd,     // UART RX <- CP2102N
    inout  wire [15:0] jp1,         // JP1 GPIO header
    output wire [1:0]  led          // onboard LEDs
);

    // --- Output enable and data registers ---
    reg [15:0] oe  = 16'h0000;     // 0 = input (hi-Z), 1 = output
    reg [15:0] od  = 16'h0000;     // output data

    // Active-low LEDs: LED0 = heartbeat, LED1 = UART activity
    reg [25:0] hb_cnt = 0;
    reg [22:0] act_cnt = 0;

    always @(posedge clk_in) hb_cnt <= hb_cnt + 1;

    assign led[0] = ~hb_cnt[25];       // ~1 Hz heartbeat
    assign led[1] = ~(act_cnt > 0);    // UART activity

    // --- Tristate JP1 pins ---
    genvar i;
    generate
        for (i = 0; i < 16; i = i + 1) begin : jp1_io
            assign jp1[i] = oe[i] ? od[i] : 1'bz;
        end
    endgenerate

    // Read JP1 inputs (active value, whether driven by us or external)
    wire [15:0] jp1_in;
    assign jp1_in = jp1;

    // --- UART TX ---
    reg       tx_start = 0;
    reg [7:0] tx_data  = 0;
    wire      tx_busy;

    uart_tx #(.CLK_FREQ(50_000_000), .BAUD(115200)) utx (
        .clk(clk_in), .start(tx_start), .data(tx_data),
        .tx(ser_txd), .busy(tx_busy)
    );

    // --- UART RX ---
    wire       rx_valid;
    wire [7:0] rx_data;

    uart_rx #(.CLK_FREQ(50_000_000), .BAUD(115200)) urx (
        .clk(clk_in), .rx(ser_rxd),
        .valid(rx_valid), .data(rx_data)
    );

    // --- Hex conversion ---
    function [7:0] hex_char;
        input [3:0] nib;
        hex_char = (nib < 10) ? (8'd48 + nib) : (8'd55 + nib); // 0-9, A-F
    endfunction

    function [3:0] from_hex;
        input [7:0] ch;
        if (ch >= "0" && ch <= "9")
            from_hex = ch - "0";
        else if (ch >= "A" && ch <= "F")
            from_hex = ch - "A" + 10;
        else if (ch >= "a" && ch <= "f")
            from_hex = ch - "a" + 10;
        else
            from_hex = 0;
    endfunction

    // --- Command state machine ---
    localparam S_IDLE     = 0,
               S_SEND_HEX = 1,  // sending 4 hex chars + \n
               S_RX_HEX   = 2,  // receiving 4 hex chars
               S_WALK     = 3;  // walking pins

    reg [3:0]  state = S_IDLE;
    reg [3:0]  send_idx = 0;     // 0-4 for hex+newline
    reg [15:0] send_val = 0;
    reg [1:0]  rx_cmd = 0;       // 0=O, 1=D
    reg [3:0]  rx_nib_cnt = 0;
    reg [15:0] rx_val = 0;

    // Walk mode
    reg [4:0]  walk_pin = 0;     // 0-15 current pin, 16+ = done
    reg [22:0] walk_timer = 0;
    reg [2:0]  walk_sub = 0;     // sub-state within each pin step

    always @(posedge clk_in) begin
        if (tx_start && !tx_busy)
            tx_start <= 0;

        // UART activity LED timer
        if (act_cnt > 0)
            act_cnt <= act_cnt - 1;

        case (state)
        S_IDLE: begin
            if (rx_valid) begin
                act_cnt <= 23'd5_000_000;
                case (rx_data)
                "R", "r": begin
                    send_val <= jp1_in;
                    send_idx <= 0;
                    state <= S_SEND_HEX;
                end
                "O", "o": begin
                    rx_cmd <= 0;
                    rx_nib_cnt <= 0;
                    rx_val <= 0;
                    state <= S_RX_HEX;
                end
                "D", "d": begin
                    rx_cmd <= 1;
                    rx_nib_cnt <= 0;
                    rx_val <= 0;
                    state <= S_RX_HEX;
                end
                "I", "i": begin
                    oe <= 16'h0000;
                    od <= 16'h0000;
                    // Send "OK\n"
                    send_val <= 16'h4F4B; // reuse send for OK
                    send_idx <= 5; // special: send OK\n
                    state <= S_SEND_HEX;
                end
                "W", "w": begin
                    // Start walk mode
                    oe <= 16'h0001;  // drive pin 0
                    od <= 16'h0001;
                    walk_pin <= 0;
                    walk_timer <= 23'd5_000_000; // 100ms settle
                    walk_sub <= 0;
                    state <= S_WALK;
                end
                default: ; // ignore
                endcase
            end
        end

        S_SEND_HEX: begin
            if (!tx_busy && !tx_start) begin
                if (send_idx == 5) begin
                    // Special: send 'O'
                    tx_data <= "O";
                    tx_start <= 1;
                    send_idx <= 6;
                end else if (send_idx == 6) begin
                    tx_data <= "K";
                    tx_start <= 1;
                    send_idx <= 7;
                end else if (send_idx == 7) begin
                    tx_data <= "\n";
                    tx_start <= 1;
                    state <= S_IDLE;
                end else if (send_idx < 4) begin
                    // Send hex nibbles MSB first
                    case (send_idx)
                    0: tx_data <= hex_char(send_val[15:12]);
                    1: tx_data <= hex_char(send_val[11:8]);
                    2: tx_data <= hex_char(send_val[7:4]);
                    3: tx_data <= hex_char(send_val[3:0]);
                    endcase
                    tx_start <= 1;
                    send_idx <= send_idx + 1;
                end else begin
                    // send_idx == 4: newline
                    tx_data <= "\n";
                    tx_start <= 1;
                    state <= S_IDLE;
                end
            end
        end

        S_RX_HEX: begin
            if (rx_valid) begin
                act_cnt <= 23'd5_000_000;
                rx_val <= {rx_val[11:0], from_hex(rx_data)};
                rx_nib_cnt <= rx_nib_cnt + 1;
                if (rx_nib_cnt == 3) begin
                    // Got 4 nibbles
                    case (rx_cmd)
                    0: oe <= {rx_val[11:0], from_hex(rx_data)};
                    1: od <= {rx_val[11:0], from_hex(rx_data)};
                    endcase
                    // Send readback
                    send_val <= jp1_in;
                    send_idx <= 0;
                    state <= S_SEND_HEX;
                end
            end
        end

        S_WALK: begin
            if (walk_timer > 0) begin
                walk_timer <= walk_timer - 1;
            end else begin
                case (walk_sub)
                0: begin
                    // Timer expired, read and send: "NN:XXXX\n"
                    // First send pin number tens digit
                    if (!tx_busy && !tx_start) begin
                        if (walk_pin < 10)
                            tx_data <= "0";
                        else
                            tx_data <= "1";
                        tx_start <= 1;
                        walk_sub <= 1;
                    end
                end
                1: begin
                    if (!tx_busy && !tx_start) begin
                        tx_data <= "0" + (walk_pin < 10 ? walk_pin[3:0] : walk_pin[3:0] - 4'd10);
                        tx_start <= 1;
                        walk_sub <= 2;
                    end
                end
                2: begin
                    if (!tx_busy && !tx_start) begin
                        tx_data <= ":";
                        tx_start <= 1;
                        send_val <= jp1_in;
                        walk_sub <= 3;
                    end
                end
                3: begin
                    // Send 4 hex chars
                    if (!tx_busy && !tx_start) begin
                        tx_data <= hex_char(send_val[15:12]);
                        tx_start <= 1;
                        walk_sub <= 4;
                    end
                end
                4: begin
                    if (!tx_busy && !tx_start) begin
                        tx_data <= hex_char(send_val[11:8]);
                        tx_start <= 1;
                        walk_sub <= 5;
                    end
                end
                5: begin
                    if (!tx_busy && !tx_start) begin
                        tx_data <= hex_char(send_val[7:4]);
                        tx_start <= 1;
                        walk_sub <= 6;
                    end
                end
                6: begin
                    if (!tx_busy && !tx_start) begin
                        tx_data <= hex_char(send_val[3:0]);
                        tx_start <= 1;
                        walk_sub <= 7;
                    end
                end
                7: begin
                    if (!tx_busy && !tx_start) begin
                        tx_data <= "\n";
                        tx_start <= 1;
                        // Next pin
                        if (walk_pin == 15) begin
                            oe <= 16'h0000;
                            od <= 16'h0000;
                            state <= S_IDLE;
                        end else begin
                            walk_pin <= walk_pin + 1;
                            oe <= (16'h0001 << (walk_pin + 1));
                            od <= (16'h0001 << (walk_pin + 1));
                            walk_timer <= 23'd5_000_000;
                            walk_sub <= 0;
                        end
                    end
                end
                endcase
            end
        end

        endcase
    end

endmodule

// --- Simple UART TX ---
module uart_tx #(
    parameter CLK_FREQ = 50_000_000,
    parameter BAUD     = 115200
)(
    input  wire       clk,
    input  wire       start,
    input  wire [7:0] data,
    output reg        tx = 1,
    output wire       busy
);
    localparam CLKS_PER_BIT = CLK_FREQ / BAUD;
    localparam BIT_W = $clog2(CLKS_PER_BIT);

    reg [3:0]      bit_idx = 0;
    reg [9:0]      shift   = 10'h3FF;
    reg [BIT_W:0]  cnt     = 0;
    reg            active  = 0;

    assign busy = active;

    always @(posedge clk) begin
        if (!active) begin
            tx <= 1;
            if (start) begin
                shift <= {1'b1, data, 1'b0}; // stop + data + start
                bit_idx <= 0;
                cnt <= 0;
                active <= 1;
            end
        end else begin
            tx <= shift[0];
            if (cnt == CLKS_PER_BIT - 1) begin
                cnt <= 0;
                shift <= {1'b1, shift[9:1]};
                if (bit_idx == 9)
                    active <= 0;
                else
                    bit_idx <= bit_idx + 1;
            end else
                cnt <= cnt + 1;
        end
    end
endmodule

// --- Simple UART RX ---
module uart_rx #(
    parameter CLK_FREQ = 50_000_000,
    parameter BAUD     = 115200
)(
    input  wire       clk,
    input  wire       rx,
    output reg        valid = 0,
    output reg  [7:0] data  = 0
);
    localparam CLKS_PER_BIT = CLK_FREQ / BAUD;
    localparam HALF_BIT     = CLKS_PER_BIT / 2;
    localparam BIT_W = $clog2(CLKS_PER_BIT);

    reg [2:0]      rx_sync = 3'b111;
    reg [3:0]      bit_idx = 0;
    reg [BIT_W:0]  cnt     = 0;
    reg [7:0]      shift   = 0;
    reg            active  = 0;

    wire rx_s = rx_sync[2];

    always @(posedge clk) begin
        rx_sync <= {rx_sync[1:0], rx};
        valid <= 0;

        if (!active) begin
            if (!rx_s) begin // start bit
                cnt <= 0;
                bit_idx <= 0;
                active <= 1;
            end
        end else begin
            if (cnt == (bit_idx == 0 ? HALF_BIT : CLKS_PER_BIT) - 1) begin
                cnt <= 0;
                if (bit_idx == 0) begin
                    // Middle of start bit
                    if (rx_s) // false start
                        active <= 0;
                    else
                        bit_idx <= 1;
                end else if (bit_idx <= 8) begin
                    shift <= {rx_s, shift[7:1]};
                    bit_idx <= bit_idx + 1;
                end else begin
                    // Stop bit
                    data <= shift;
                    valid <= 1;
                    active <= 0;
                end
            end else
                cnt <= cnt + 1;
        end
    end
endmodule
