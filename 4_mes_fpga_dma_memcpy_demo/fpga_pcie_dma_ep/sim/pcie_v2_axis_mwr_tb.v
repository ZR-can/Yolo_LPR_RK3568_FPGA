`timescale 1ns/1ps

module pcie_v2_axis_mwr_tb;

localparam [31:0] CMD_MAGIC       = 32'h56325043;
localparam [31:0] RESP_MAGIC      = 32'h52325043;
localparam [31:0] FRAME_MAGIC     = 32'h46325043;
localparam [31:0] OP_VIDEO_START  = 32'h0000_0002;
localparam [31:0] VIDEO_WIDTH     = 32'd640;
localparam [31:0] VIDEO_HEIGHT    = 32'd640;
localparam [31:0] VIDEO_STRIDE    = 32'd1920;
localparam [31:0] VIDEO_BYTES     = 32'd1228800;
localparam [31:0] FRAME_DESC_SIZE = 32'd64;
localparam [31:0] SLOT_SIZE       = 32'd1228864;
localparam [63:0] RESPONSE_ADDR   = 64'h0000_0000_0000_1000;
localparam [63:0] RING_BASE       = 64'h0000_0000_0000_2000;
localparam [31:0] SEQ             = 32'h0000_005a;

reg clk;
reg rst_n;
reg [7:0] i_cfg_pbus_num;
reg [4:0] i_cfg_pbus_dev_num;
reg [2:0] i_cfg_max_payload_size;
reg i_bar0_wr_en;
reg [11:0] i_bar0_wr_addr;
reg [127:0] i_bar0_wr_data;
reg [15:0] i_bar0_wr_byte_en;
reg i_axis_tready;

wire o_axis_tvalid;
wire [127:0] o_axis_tdata;
wire o_axis_tlast;
wire o_axis_tuser;
wire [31:0] o_heartbeat;
wire [31:0] o_frame_id;
wire o_video_enable;
wire [31:0] o_error_flags;

integer packet_count;
integer total_bytes;
integer beat;
integer beats;
integer timeout_cycles;
reg [63:0] expected_addr;
reg [9:0] dw_len;
reg [63:0] pkt_addr;
reg [127:0] axis_data;
reg [127:0] decoded_data;
reg [127:0] expected_data;
reg axis_last;
reg [31:0] checksum;
reg [31:0] expected_checksum;

pcie_v2_axis_mwr #(
    .ADDR_WIDTH(12)
) dut (
    .clk(clk),
    .rst_n(rst_n),
    .i_cfg_pbus_num(i_cfg_pbus_num),
    .i_cfg_pbus_dev_num(i_cfg_pbus_dev_num),
    .i_cfg_max_payload_size(i_cfg_max_payload_size),
    .i_bar0_wr_en(i_bar0_wr_en),
    .i_bar0_wr_addr(i_bar0_wr_addr),
    .i_bar0_wr_data(i_bar0_wr_data),
    .i_bar0_wr_byte_en(i_bar0_wr_byte_en),
    .i_axis_tready(i_axis_tready),
    .o_axis_tvalid(o_axis_tvalid),
    .o_axis_tdata(o_axis_tdata),
    .o_axis_tlast(o_axis_tlast),
    .o_axis_tuser(o_axis_tuser),
    .o_heartbeat(o_heartbeat),
    .o_frame_id(o_frame_id),
    .o_video_enable(o_video_enable),
    .o_error_flags(o_error_flags)
);

initial begin
    clk = 1'b0;
    forever #4 clk = ~clk;
end

function [127:0] endian_convert;
    input [127:0] data_in;
    begin
        endian_convert[31:0]   = {data_in[7:0], data_in[15:8], data_in[23:16], data_in[31:24]};
        endian_convert[63:32]  = {data_in[39:32], data_in[47:40], data_in[55:48], data_in[63:56]};
        endian_convert[95:64]  = {data_in[71:64], data_in[79:72], data_in[87:80], data_in[95:88]};
        endian_convert[127:96] = {data_in[103:96], data_in[111:104], data_in[119:112], data_in[127:120]};
    end
endfunction

function [31:0] command_checksum;
    input [31:0] op;
    input [31:0] seq;
    input [63:0] response_addr;
    input [31:0] slot_size;
    input [31:0] slot_count;
    input [63:0] ring_base;
    begin
        command_checksum = CMD_MAGIC ^ op ^ seq ^ 32'd64 ^
                           response_addr[31:0] ^ response_addr[63:32] ^
                           slot_size ^ slot_count ^
                           ring_base[31:0] ^ ring_base[63:32];
    end
endfunction

function [31:0] response_checksum;
    input [31:0] seq;
    input [31:0] heartbeat;
    input [31:0] status;
    input [31:0] frame_id;
    input [31:0] frame_bytes;
    input [31:0] error_flags;
    begin
        response_checksum = RESP_MAGIC ^ seq ^ heartbeat ^ status ^
                            frame_id ^ frame_bytes ^ error_flags;
    end
endfunction

function [31:0] frame_checksum;
    input [31:0] frame_id;
    begin
        frame_checksum = FRAME_MAGIC ^ frame_id ^ VIDEO_BYTES ^
                         VIDEO_WIDTH ^ VIDEO_HEIGHT ^ VIDEO_STRIDE;
    end
endfunction

function [31:0] rgb888_dword;
    input [7:0] marker;
    input [31:0] frame_no;
    begin
        rgb888_dword = {8'h80, frame_no[7:0], marker,
                        marker ^ frame_no[7:0]};
    end
endfunction

function [127:0] expected_video_payload;
    input [31:0] frame_offset;
    input [9:0] payload_beat;
    input [31:0] frame_no;
    reg [7:0] marker;
    begin
        marker = frame_offset[11:4] ^ payload_beat[7:0];
        expected_video_payload = {
            rgb888_dword(marker ^ 8'h0c, frame_no),
            rgb888_dword(marker ^ 8'h08, frame_no),
            rgb888_dword(marker ^ 8'h04, frame_no),
            rgb888_dword(marker, frame_no)
        };
    end
endfunction

function [9:0] beats_from_dw;
    input [9:0] dw_count;
    begin
        beats_from_dw = (dw_count + 10'd3) >> 2;
    end
endfunction

task fail;
    input [1023:0] message;
    begin
        $display("V2_AXIS_MWR_TB_FAIL: %0s", message);
        $finish;
    end
endtask

task bar0_write;
    input [11:0] addr;
    input [127:0] data;
    begin
        @(negedge clk);
        i_bar0_wr_addr = addr;
        i_bar0_wr_data = data;
        i_bar0_wr_byte_en = 16'hffff;
        i_bar0_wr_en = 1'b1;
        @(negedge clk);
        i_bar0_wr_en = 1'b0;
        i_bar0_wr_addr = 12'd0;
        i_bar0_wr_data = 128'd0;
        i_bar0_wr_byte_en = 16'd0;
    end
endtask

task send_video_start;
    begin
        checksum = command_checksum(OP_VIDEO_START, SEQ, RESPONSE_ADDR,
                                    SLOT_SIZE, 32'd2, RING_BASE);
        bar0_write(12'd0, {32'd64, SEQ, OP_VIDEO_START, CMD_MAGIC});
        bar0_write(12'd1, {32'd2, SLOT_SIZE, RESPONSE_ADDR});
        bar0_write(12'd2, {32'd0, 32'd0, RING_BASE});
        bar0_write(12'd3, {96'd0, checksum});
    end
endtask

task recv_axis_word;
    output [127:0] data;
    output last;
    begin
        timeout_cycles = 0;
        while (!o_axis_tvalid) begin
            @(negedge clk);
            timeout_cycles = timeout_cycles + 1;
            if (timeout_cycles > 1000000) begin
                $display("timeout context: packets=%0d total_bytes=%0d video_enable=%0b frame_id=%0d errors=0x%08x",
                         packet_count, total_bytes, o_video_enable,
                         o_frame_id, o_error_flags);
                fail("timeout waiting for AXIS word");
            end
        end
        data = o_axis_tdata;
        last = o_axis_tlast;
        @(posedge clk);
        @(negedge clk);
    end
endtask

task recv_packet_header;
    output [63:0] addr;
    output [9:0] len_dw;
    begin
        recv_axis_word(axis_data, axis_last);
        if (axis_last)
            fail("packet header asserted tlast");
        addr = {axis_data[95:64], axis_data[127:96]};
        len_dw = axis_data[9:0];
    end
endtask

task recv_payload;
    output [127:0] data;
    output last;
    begin
        recv_axis_word(axis_data, last);
        data = endian_convert(axis_data);
    end
endtask

initial begin
    rst_n = 1'b0;
    i_cfg_pbus_num = 8'h01;
    i_cfg_pbus_dev_num = 5'h00;
    i_cfg_max_payload_size = 3'd3;
    i_bar0_wr_en = 1'b0;
    i_bar0_wr_addr = 12'd0;
    i_bar0_wr_data = 128'd0;
    i_bar0_wr_byte_en = 16'd0;
    i_axis_tready = 1'b1;

    repeat (8) @(posedge clk);
    rst_n = 1'b1;
    repeat (8) @(posedge clk);

    send_video_start();

    recv_packet_header(pkt_addr, dw_len);
    if (pkt_addr !== RESPONSE_ADDR || dw_len !== 10'd8)
        fail("unexpected response packet header");

    recv_payload(decoded_data, axis_last);
    if (axis_last)
        fail("response beat 0 asserted tlast");
    if (decoded_data[31:0] !== RESP_MAGIC || decoded_data[63:32] !== SEQ)
        fail("bad response payload 0");

    recv_payload(axis_data, axis_last);
    if (!axis_last)
        fail("response beat 1 did not assert tlast");
    expected_checksum = response_checksum(decoded_data[63:32],
                                          decoded_data[95:64],
                                          axis_data[31:0],
                                          axis_data[63:32],
                                          axis_data[95:64],
                                          axis_data[127:96]);
    if (decoded_data[127:96] !== expected_checksum ||
        axis_data[31:0] !== 32'h0000_0003 ||
        axis_data[63:32] !== 32'd0 ||
        axis_data[95:64] !== VIDEO_BYTES ||
        axis_data[127:96] !== 32'd0)
        fail("bad response checksum or status");
    $display("response packet ok");

    expected_addr = RING_BASE + FRAME_DESC_SIZE;
    total_bytes = 0;
    packet_count = 0;

    while (total_bytes < VIDEO_BYTES) begin
        recv_packet_header(pkt_addr, dw_len);
        if (pkt_addr !== expected_addr)
            fail("unexpected video packet address");
        if (dw_len == 10'd0 || dw_len > 10'd256)
            fail("invalid video packet DW length");
        if ((pkt_addr[11:0] + {dw_len, 2'b00}) > 13'd4096)
            fail("video packet crosses 4KB boundary");

        beats = beats_from_dw(dw_len);
        for (beat = 0; beat < beats; beat = beat + 1) begin
            recv_payload(decoded_data, axis_last);
            expected_data = expected_video_payload(total_bytes[31:0],
                                                   beat[9:0], 32'd0);
            if (decoded_data !== expected_data)
                fail("video payload mismatch");
            if (axis_last !== (beat == beats - 1))
                fail("bad video packet tlast");
        end

        total_bytes = total_bytes + {dw_len, 2'b00};
        expected_addr = expected_addr + {54'd0, dw_len, 2'b00};
        packet_count = packet_count + 1;
        if ((packet_count % 200) == 0)
            $display("video progress: packets=%0d total_bytes=%0d",
                     packet_count, total_bytes);
        if (total_bytes > VIDEO_BYTES)
            fail("video byte count overflow");
    end

    $display("video payload complete: packets=%0d total_bytes=%0d",
             packet_count, total_bytes);

    recv_packet_header(pkt_addr, dw_len);
    if (pkt_addr !== RING_BASE || dw_len !== 10'd8)
        fail("unexpected frame descriptor header");

    recv_payload(decoded_data, axis_last);
    if (axis_last)
        fail("frame descriptor beat 0 asserted tlast");
    if (decoded_data[31:0] !== FRAME_MAGIC ||
        decoded_data[63:32] !== VIDEO_BYTES ||
        decoded_data[95:64] !== 32'd0 ||
        decoded_data[127:96] !== frame_checksum(32'd0))
        fail("bad frame descriptor payload 0");

    recv_payload(axis_data, axis_last);
    if (!axis_last)
        fail("frame descriptor beat 1 did not assert tlast");
    if (axis_data[31:0] !== 32'd0 ||
        axis_data[63:32] !== VIDEO_WIDTH ||
        axis_data[95:64] !== VIDEO_HEIGHT ||
        axis_data[127:96] !== VIDEO_STRIDE)
        fail("bad frame descriptor payload 1");

    $display("V2_AXIS_MWR_TB_PASS: packets=%0d video_bytes=%0d frame_id=%0d",
             packet_count, total_bytes, o_frame_id);
    $finish;
end

endmodule
