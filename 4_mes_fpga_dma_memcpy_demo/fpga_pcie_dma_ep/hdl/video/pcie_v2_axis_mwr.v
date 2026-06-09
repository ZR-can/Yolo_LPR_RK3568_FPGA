// V2 RK3568 <-> FPGA PCIe communication and RGB888 test-video MWr engine.
// This module observes BAR0 mailbox writes and injects FPGA-initiated MWr TLPs
// on the PCIe axis_slave2 path. The legacy DMA/PIO datapath remains intact.

module pcie_v2_axis_mwr #(
    parameter integer ADDR_WIDTH = 12
)(
    input              clk,
    input              rst_n,

    input      [7:0]   i_cfg_pbus_num,
    input      [4:0]   i_cfg_pbus_dev_num,
    input      [2:0]   i_cfg_max_payload_size,

    input              i_bar0_wr_en,
    input      [ADDR_WIDTH-1:0] i_bar0_wr_addr,
    input      [127:0] i_bar0_wr_data,
    input      [15:0]  i_bar0_wr_byte_en,

    input              i_axis_tready,
    output reg         o_axis_tvalid,
    output reg [127:0] o_axis_tdata,
    output reg         o_axis_tlast,
    output reg         o_axis_tuser,

    output reg [31:0]  o_heartbeat,
    output reg [31:0]  o_frame_id,
    output reg         o_video_enable,
    output reg [31:0]  o_error_flags
);

localparam [31:0] CMD_MAGIC       = 32'h56325043; // "V2PC"
localparam [31:0] RESP_MAGIC      = 32'h52325043; // "R2PC"
localparam [31:0] FRAME_MAGIC     = 32'h46325043; // "F2PC"
localparam [31:0] OP_PING         = 32'h0000_0001;
localparam [31:0] OP_VIDEO_START  = 32'h0000_0002;
localparam [31:0] OP_VIDEO_STOP   = 32'h0000_0003;

localparam [31:0] VIDEO_WIDTH     = 32'd640;
localparam [31:0] VIDEO_HEIGHT    = 32'd640;
localparam [31:0] VIDEO_STRIDE    = 32'd1920;
localparam [31:0] VIDEO_BYTES     = 32'd1228800;
localparam [31:0] FRAME_DESC_SIZE = 32'd64;
localparam [31:0] DEFAULT_SLOT_SZ = 32'd1228864;
localparam [31:0] FRAME_INTERVAL  = 32'd4166667; // 125 MHz / 30 fps

localparam [1:0] ST_IDLE = 2'd0;
localparam [1:0] ST_HDR  = 2'd1;
localparam [1:0] ST_DATA = 2'd2;

localparam [1:0] PKT_RESP  = 2'd1;
localparam [1:0] PKT_DESC  = 2'd2;
localparam [1:0] PKT_VIDEO = 2'd3;

reg [1:0]  state;
reg [1:0]  pkt_type;
reg [63:0] pkt_addr;
reg [9:0]  pkt_dw_len;
reg [9:0]  pkt_beats;
reg [9:0]  beat_idx;
reg [31:0] pkt_frame_offset;
reg [31:0] pkt_frame_id;
reg [7:0]  tag;

reg [31:0] cmd_magic;
reg [31:0] cmd_op;
reg [31:0] cmd_seq;
reg [31:0] cmd_payload_len;
reg [63:0] cmd_response_addr;
reg [31:0] cmd_slot_size;
reg [31:0] cmd_slot_count;
reg [63:0] cmd_ring_base;
reg [31:0] cmd_control;
reg [31:0] cmd_checksum;
reg        cmd_word0_seen;
reg        cmd_word1_seen;
reg        cmd_word2_seen;
reg        response_pending;
reg        cmd_eval_pending;
reg        cmd_start_video_pulse;
reg        cmd_stop_video_pulse;
reg        cmd_bad_op_pulse;
reg        cmd_bad_checksum_pulse;
reg [63:0] cmd_video_base_latched;
reg [31:0] cmd_video_slot_size_latched;
reg [31:0] cmd_video_slot_count_latched;

reg [63:0] ring_base;
reg [31:0] slot_size;
reg [31:0] slot_count;
reg [31:0] write_index;
reg [31:0] frame_timer;
reg        frame_active;
reg        frame_desc_pending;
reg [2:0]  cfg_max_payload_size_q;
reg [63:0] frame_slot_addr;
reg [63:0] next_slot_addr;
reg [63:0] video_addr;
reg [31:0] video_remaining;
reg [31:0] video_offset;

wire axis_fire;
wire [9:0] max_payload_dw;
wire [9:0] video_chunk_dw;
wire [31:0] command_checksum;
wire [31:0] response_checksum;
wire [31:0] response_status;
wire [31:0] frame_checksum;
wire [15:0] requester_id;
wire [31:0] mwr_header_tx;
wire [7:0]  dwbe;
wire        mailbox_word0_wr;
wire        mailbox_word1_wr;
wire        mailbox_word2_wr;
wire        mailbox_word3_wr;
wire [63:0] cmd_video_base;
wire [31:0] cmd_video_slot_size;
wire [31:0] cmd_video_slot_count;

assign axis_fire = o_axis_tvalid && i_axis_tready;
assign requester_id = {i_cfg_pbus_num, i_cfg_pbus_dev_num, 3'b0};
assign mwr_header_tx = {8'h60, 1'b0, 3'b0, 1'b0, 1'b0, 1'b0,
                        1'b0, 1'b0, 1'b0, 2'b0, 2'b0, pkt_dw_len};
assign dwbe = 8'hff;
assign mailbox_word0_wr = i_bar0_wr_en && (i_bar0_wr_addr == {ADDR_WIDTH{1'b0}}) && &i_bar0_wr_byte_en;
assign mailbox_word1_wr = i_bar0_wr_en && (i_bar0_wr_addr == {{(ADDR_WIDTH-1){1'b0}}, 1'b1}) && &i_bar0_wr_byte_en;
assign mailbox_word2_wr = i_bar0_wr_en && (i_bar0_wr_addr == {{(ADDR_WIDTH-2){1'b0}}, 2'd2}) && &i_bar0_wr_byte_en;
assign mailbox_word3_wr = i_bar0_wr_en && (i_bar0_wr_addr == {{(ADDR_WIDTH-2){1'b0}}, 2'd3}) && &i_bar0_wr_byte_en;
assign cmd_video_base = (cmd_ring_base != 64'd0) ? cmd_ring_base :
                        (cmd_response_addr + 64'd8192);
assign cmd_video_slot_size = (cmd_slot_size >= DEFAULT_SLOT_SZ) ? cmd_slot_size : DEFAULT_SLOT_SZ;
assign cmd_video_slot_count = (cmd_slot_count == 32'd0) ? 32'd2 : cmd_slot_count;

assign max_payload_dw = (cfg_max_payload_size_q == 3'd0) ? 10'h020 :
                        (cfg_max_payload_size_q == 3'd1) ? 10'h040 :
                        (cfg_max_payload_size_q == 3'd2) ? 10'h080 :
                        (cfg_max_payload_size_q == 3'd3) ? 10'h100 : 10'h020;

assign command_checksum = CMD_MAGIC ^ cmd_op ^ cmd_seq ^ cmd_payload_len ^
                          cmd_response_addr[31:0] ^ cmd_response_addr[63:32] ^
                          cmd_slot_size ^ cmd_slot_count ^
                          cmd_ring_base[31:0] ^ cmd_ring_base[63:32] ^
                          cmd_control;

assign response_status = {30'b0, o_video_enable, 1'b1};

assign response_checksum = RESP_MAGIC ^ cmd_seq ^ o_heartbeat ^
                           response_status ^ o_frame_id ^ VIDEO_BYTES ^
                           o_error_flags;

assign frame_checksum = FRAME_MAGIC ^ o_frame_id ^ VIDEO_BYTES ^ VIDEO_WIDTH ^
                        VIDEO_HEIGHT ^ VIDEO_STRIDE;

function [127:0] endian_convert;
    input [127:0] data_in;
    begin
        endian_convert[31:0]   = {data_in[7:0], data_in[15:8], data_in[23:16], data_in[31:24]};
        endian_convert[63:32]  = {data_in[39:32], data_in[47:40], data_in[55:48], data_in[63:56]};
        endian_convert[95:64]  = {data_in[71:64], data_in[79:72], data_in[87:80], data_in[95:88]};
        endian_convert[127:96] = {data_in[103:96], data_in[111:104], data_in[119:112], data_in[127:120]};
    end
endfunction

function [31:0] rgb888_dword;
    input [7:0]  marker;
    input [31:0] frame_no;
    begin
        rgb888_dword = {8'h80, frame_no[7:0], marker,
                        marker ^ frame_no[7:0]};
    end
endfunction

function [127:0] video_payload;
    input [31:0] byte_offset;
    input [9:0]  payload_beat;
    input [31:0] frame_no;
    reg [7:0] marker;
    begin
        marker = byte_offset[11:4] ^ payload_beat[7:0];
        video_payload = {
            rgb888_dword(marker ^ 8'h0c, frame_no),
            rgb888_dword(marker ^ 8'h08, frame_no),
            rgb888_dword(marker ^ 8'h04, frame_no),
            rgb888_dword(marker, frame_no)
        };
    end
endfunction

function [127:0] response_payload0;
    input unused;
    begin
        response_payload0 = {response_checksum, o_heartbeat, cmd_seq, RESP_MAGIC};
    end
endfunction

function [127:0] response_payload1;
    input unused;
    begin
        response_payload1 = {o_error_flags, VIDEO_BYTES, o_frame_id,
                             response_status};
    end
endfunction

function [127:0] frame_desc_payload0;
    input unused;
    begin
        frame_desc_payload0 = {frame_checksum, pkt_frame_id, VIDEO_BYTES, FRAME_MAGIC};
    end
endfunction

function [127:0] frame_desc_payload1;
    input unused;
    begin
        frame_desc_payload1 = {VIDEO_STRIDE, VIDEO_HEIGHT, VIDEO_WIDTH,
                               {16'b0, write_index[15:0]}};
    end
endfunction

function [127:0] packet_payload;
    input [1:0]  payload_type;
    input [9:0]  payload_beat;
    input [31:0] frame_offset;
    input [31:0] frame_no;
    begin
        if (payload_type == PKT_RESP) begin
            packet_payload = (payload_beat == 10'd0) ? response_payload0(1'b0) : response_payload1(1'b0);
        end else if (payload_type == PKT_DESC) begin
            packet_payload = (payload_beat == 10'd0) ? frame_desc_payload0(1'b0) : frame_desc_payload1(1'b0);
        end else begin
            packet_payload = video_payload(frame_offset, payload_beat, frame_no);
        end
    end
endfunction

function [9:0] bytes_to_dw;
    input [31:0] byte_count;
    begin
        bytes_to_dw = byte_count[11:2];
    end
endfunction

function [9:0] beats_from_dw;
    input [9:0] dw_count;
    begin
        beats_from_dw = (dw_count + 10'd3) >> 2;
    end
endfunction

function [9:0] calc_video_chunk_dw;
    input [63:0] addr;
    input [31:0] remaining_bytes;
    input [9:0]  max_dw;
    reg [12:0] bytes_to_4kb;
    reg [9:0]  boundary_dw;
    reg [9:0]  remain_dw;
    begin
        bytes_to_4kb = 13'd4096 - {1'b0, addr[11:0]};
        boundary_dw = bytes_to_4kb[11:2];
        if (remaining_bytes >= {20'd0, max_dw, 2'b0})
            remain_dw = max_dw;
        else
            remain_dw = remaining_bytes[11:2];
        if (boundary_dw == 10'd0)
            boundary_dw = max_dw;

        if (remain_dw <= max_dw && remain_dw <= boundary_dw)
            calc_video_chunk_dw = remain_dw;
        else if (max_dw <= boundary_dw)
            calc_video_chunk_dw = max_dw;
        else
            calc_video_chunk_dw = boundary_dw;
    end
endfunction

assign video_chunk_dw = calc_video_chunk_dw(video_addr, video_remaining, max_payload_dw);

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        o_heartbeat <= 32'd0;
        cfg_max_payload_size_q <= 3'd0;
    end else begin
        o_heartbeat <= o_heartbeat + 32'd1;
        cfg_max_payload_size_q <= i_cfg_max_payload_size;
    end
end

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        cmd_magic <= 32'd0;
        cmd_op <= 32'd0;
        cmd_seq <= 32'd0;
        cmd_payload_len <= 32'd0;
        cmd_response_addr <= 64'd0;
        cmd_slot_size <= DEFAULT_SLOT_SZ;
        cmd_slot_count <= 32'd2;
        cmd_ring_base <= 64'd0;
        cmd_control <= 32'd0;
        cmd_checksum <= 32'd0;
        cmd_word0_seen <= 1'b0;
        cmd_word1_seen <= 1'b0;
        cmd_word2_seen <= 1'b0;
        response_pending <= 1'b0;
        cmd_eval_pending <= 1'b0;
        cmd_start_video_pulse <= 1'b0;
        cmd_stop_video_pulse <= 1'b0;
        cmd_bad_op_pulse <= 1'b0;
        cmd_bad_checksum_pulse <= 1'b0;
        cmd_video_base_latched <= 64'd0;
        cmd_video_slot_size_latched <= DEFAULT_SLOT_SZ;
        cmd_video_slot_count_latched <= 32'd2;
    end else begin
        cmd_start_video_pulse <= 1'b0;
        cmd_stop_video_pulse <= 1'b0;
        cmd_bad_op_pulse <= 1'b0;
        cmd_bad_checksum_pulse <= 1'b0;

        if (state == ST_HDR && axis_fire && pkt_type == PKT_RESP)
            response_pending <= 1'b0;

        if (cmd_eval_pending) begin
            response_pending <= 1'b1;
            cmd_eval_pending <= 1'b0;
            if (cmd_checksum == command_checksum) begin
                if (cmd_op == OP_VIDEO_START) begin
                    cmd_start_video_pulse <= 1'b1;
                    cmd_video_base_latched <= cmd_video_base;
                    cmd_video_slot_size_latched <= cmd_video_slot_size;
                    cmd_video_slot_count_latched <= cmd_video_slot_count;
                end else if (cmd_op == OP_VIDEO_STOP) begin
                    cmd_stop_video_pulse <= 1'b1;
                end else if (cmd_op != OP_PING) begin
                    cmd_bad_op_pulse <= 1'b1;
                end
            end else begin
                cmd_bad_checksum_pulse <= 1'b1;
            end
        end

        if (mailbox_word0_wr) begin
            cmd_magic <= i_bar0_wr_data[31:0];
            cmd_op <= i_bar0_wr_data[63:32];
            cmd_seq <= i_bar0_wr_data[95:64];
            cmd_payload_len <= i_bar0_wr_data[127:96];
            cmd_word0_seen <= (i_bar0_wr_data[31:0] == CMD_MAGIC);
            cmd_word1_seen <= 1'b0;
            cmd_word2_seen <= 1'b0;
        end

        if (mailbox_word1_wr) begin
            cmd_response_addr <= i_bar0_wr_data[63:0];
            cmd_slot_size <= i_bar0_wr_data[95:64];
            cmd_slot_count <= i_bar0_wr_data[127:96];
            cmd_word1_seen <= cmd_word0_seen;
            cmd_word2_seen <= 1'b0;
        end

        if (mailbox_word2_wr) begin
            cmd_ring_base <= i_bar0_wr_data[63:0];
            cmd_control <= i_bar0_wr_data[95:64];
            cmd_word2_seen <= cmd_word0_seen && cmd_word1_seen;
        end

        if (mailbox_word3_wr) begin
            cmd_checksum <= i_bar0_wr_data[31:0];
            cmd_eval_pending <= cmd_word0_seen && cmd_word1_seen &&
                                cmd_word2_seen && cmd_response_addr != 64'd0;
        end
    end
end

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        frame_timer <= 32'd0;
        frame_active <= 1'b0;
        frame_desc_pending <= 1'b0;
        frame_slot_addr <= 64'd0;
        next_slot_addr <= 64'd0;
        video_addr <= 64'd0;
        video_remaining <= 32'd0;
        video_offset <= 32'd0;
        o_frame_id <= 32'd0;
        write_index <= 32'd0;
        o_video_enable <= 1'b0;
        ring_base <= 64'd0;
        slot_size <= DEFAULT_SLOT_SZ;
        slot_count <= 32'd2;
        o_error_flags <= 32'd0;
    end else begin
        if (cmd_start_video_pulse) begin
            o_video_enable <= 1'b1;
            ring_base <= cmd_video_base_latched;
            slot_size <= cmd_video_slot_size_latched;
            slot_count <= cmd_video_slot_count_latched;
            o_error_flags <= 32'd0;
            frame_timer <= FRAME_INTERVAL;
            frame_active <= 1'b0;
            frame_desc_pending <= 1'b0;
            write_index <= 32'd0;
            next_slot_addr <= cmd_video_base_latched;
        end else if (cmd_stop_video_pulse) begin
            o_video_enable <= 1'b0;
            frame_active <= 1'b0;
            frame_desc_pending <= 1'b0;
        end else if (cmd_bad_op_pulse) begin
            o_error_flags[0] <= 1'b1;
        end else if (cmd_bad_checksum_pulse) begin
            o_error_flags[1] <= 1'b1;
        end else if (o_video_enable && !frame_active && !frame_desc_pending) begin
            if (frame_timer >= FRAME_INTERVAL) begin
                frame_timer <= 32'd0;
                frame_active <= 1'b1;
                frame_desc_pending <= 1'b0;
                frame_slot_addr <= next_slot_addr;
                video_addr <= next_slot_addr + {32'd0, FRAME_DESC_SIZE};
                video_remaining <= VIDEO_BYTES;
                video_offset <= 32'd0;
            end else begin
                frame_timer <= frame_timer + 32'd1;
            end
        end

        if (!o_video_enable && !cmd_start_video_pulse) begin
            frame_timer <= 32'd0;
            frame_active <= 1'b0;
            frame_desc_pending <= 1'b0;
        end

        if (state == ST_DATA && axis_fire && o_axis_tlast && pkt_type == PKT_VIDEO) begin
            video_addr <= video_addr + {52'd0, pkt_dw_len, 2'b0};
            video_offset <= video_offset + {20'd0, pkt_dw_len, 2'b0};
            video_remaining <= video_remaining - {20'd0, pkt_dw_len, 2'b0};
            if (video_remaining <= {20'd0, pkt_dw_len, 2'b0}) begin
                frame_active <= 1'b0;
                frame_desc_pending <= 1'b1;
            end
        end

        if (state == ST_DATA && axis_fire && o_axis_tlast && pkt_type == PKT_DESC) begin
            frame_desc_pending <= 1'b0;
            o_frame_id <= o_frame_id + 32'd1;
            if (write_index + 32'd1 >= slot_count) begin
                write_index <= 32'd0;
                next_slot_addr <= ring_base;
            end else begin
                write_index <= write_index + 32'd1;
                next_slot_addr <= next_slot_addr + {32'd0, slot_size};
            end
        end
    end
end

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state <= ST_IDLE;
        pkt_type <= 2'd0;
        pkt_addr <= 64'd0;
        pkt_dw_len <= 10'd0;
        pkt_beats <= 10'd0;
        beat_idx <= 10'd0;
        pkt_frame_offset <= 32'd0;
        pkt_frame_id <= 32'd0;
        tag <= 8'd0;
        o_axis_tvalid <= 1'b0;
        o_axis_tdata <= 128'd0;
        o_axis_tlast <= 1'b0;
        o_axis_tuser <= 1'b0;
    end else begin
        case (state)
            ST_IDLE: begin
                o_axis_tvalid <= 1'b0;
                o_axis_tdata <= 128'd0;
                o_axis_tlast <= 1'b0;
                o_axis_tuser <= 1'b0;
                beat_idx <= 10'd0;

                if (response_pending) begin
                    pkt_type <= PKT_RESP;
                    pkt_addr <= cmd_response_addr;
                    pkt_dw_len <= 10'd8;
                    pkt_beats <= 10'd2;
                    pkt_frame_offset <= 32'd0;
                    pkt_frame_id <= o_frame_id;
                    state <= ST_HDR;
                end else if (frame_desc_pending) begin
                    pkt_type <= PKT_DESC;
                    pkt_addr <= frame_slot_addr;
                    pkt_dw_len <= 10'd8;
                    pkt_beats <= 10'd2;
                    pkt_frame_offset <= 32'd0;
                    pkt_frame_id <= o_frame_id;
                    state <= ST_HDR;
                end else if (frame_active && (video_remaining != 32'd0) && (video_chunk_dw != 10'd0)) begin
                    pkt_type <= PKT_VIDEO;
                    pkt_addr <= video_addr;
                    pkt_dw_len <= video_chunk_dw;
                    pkt_beats <= beats_from_dw(video_chunk_dw);
                    pkt_frame_offset <= video_offset;
                    pkt_frame_id <= o_frame_id;
                    state <= ST_HDR;
                end
            end

            ST_HDR: begin
                o_axis_tvalid <= 1'b1;
                o_axis_tlast <= 1'b0;
                o_axis_tuser <= 1'b0;
                o_axis_tdata <= {{pkt_addr[31:2], 2'b0}, pkt_addr[63:32],
                                 requester_id, tag, dwbe, mwr_header_tx};
                if (axis_fire) begin
                    state <= ST_DATA;
                    tag <= tag + 8'd1;
                    beat_idx <= 10'd0;
                    o_axis_tdata <= endian_convert(packet_payload(pkt_type, 10'd0,
                                             pkt_frame_offset, pkt_frame_id));
                    o_axis_tlast <= (pkt_beats == 10'd1);
                end
            end

            ST_DATA: begin
                o_axis_tvalid <= 1'b1;
                o_axis_tuser <= 1'b0;
                if (axis_fire) begin
                    if (beat_idx == (pkt_beats - 10'd1)) begin
                        state <= ST_IDLE;
                        o_axis_tvalid <= 1'b0;
                        o_axis_tlast <= 1'b0;
                        o_axis_tdata <= 128'd0;
                    end else begin
                        beat_idx <= beat_idx + 10'd1;
                        o_axis_tdata <= endian_convert(packet_payload(pkt_type, beat_idx + 10'd1,
                                                 pkt_frame_offset, pkt_frame_id));
                        o_axis_tlast <= ((beat_idx + 10'd1) == (pkt_beats - 10'd1));
                    end
                end
            end

            default: begin
                state <= ST_IDLE;
            end
        endcase
    end
end

endmodule
