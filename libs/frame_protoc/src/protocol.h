#pragma once
#include "types.h"
#include <cstddef>

/*
*
*   Version 4 bit   协议版本号，当前版本为 0x01
    SeqLen  4 bit   Sequence Number 字段的字节数，取值 1~8,根据Sequence Number 的增加而调整,比如小于0xff为1,0xff~0xffff为2,0xffff~0xffffff 为3 ....
    Msg Type    1 byte  消息类型 (0x01 数据包, 0x02 命令包用于nack请求,0x03用于传递rtt\丢包率\fec恢复率\nack 恢复率,0x04用于心跳及探测rtt,0x05用于回复心跳及探测rtt,0x06关键帧请求PLI, 其他留作备用)
    Sequence Number 1~8 byte    数据包或命令包各自的全局序号，从 1 开始递增，长度由 SeqLen 决定，大端序

    Msg Type为0x01时:
    Frame ID    2 byte  帧标识符，每帧递增，用于接收端匹配同一帧的所有块，大端序
    Data Block Count    1 byte  当前 FEC 分组中的数据块总数
    FEC Block Count 1 byte  当前 FEC 分组中的校验块数量
    Block Index 1 byte  当前块在所属 FEC 分组中的索引，从 0 开始
    FEC Group Index 1 byte  当前包所属的 FEC 分组编号，从 0 开始
    FEC Group Count 1 byte  当前帧包含的 FEC 分组总数
    Frame Type  1 byte  帧类型，标识音频/视频等先保留
    Frame Size  4 byte  原始帧的完整大小（字节），大端序
    Retry Count 4 bit   当前包的重传次数，0 表示首次发送，最大 15
    Payload Length  12 bit  当前包负载数据长度（字节），最大 4095，大端序
    Payload 变长  实际数据负载

    Msg Type为0x02时:
    Payload Length  16bit  当前包负载数据长度（字节）
    8byte 丢失的开始序号, 32bit mark位 用于标记 开始序号后哪些包丢失,
    8byte 丢失的开始序号, 32bit mark位 用于标记 开始序号后哪些包丢失,
    ....

    Msg Type为0x03时:
    Payload Length  16bit  当前包负载数据长度（字节）
    1byte type,1byte len,n byte data
    type:0x01 rtt 占2byte
    type:0x02 丢包率 占1byte
    type:0x03 fec恢复率 占1byte
    type:0x04 nack恢复率 占1byte
    type:0x05 带宽估计值 占4byte
    type:0x06 已经收到的完备序号,用于发送端清除历史 占8byte
    ...

    Msg Type为0x04时:
    Payload Length  16bit  当前包负载数据长度（字节）
    1byte type,1byte len,n byte data
    type:0x06 当前系统时间 占8byte
    ...

    Msg Type为0x05时:
    Payload Length  16bit  当前包负载数据长度（字节）
    1byte type,1byte len,n byte data
    type:0x07 当前系统时间 占8byte
    ...

    Msg Type为0x06时(PLI 关键帧请求):
    Payload Length  16bit  固定为 0（当前无负载，保留 TLV 扩展能力）
 */
namespace fec_protocol {

// Common header (all message types)
struct CommonHeader {
    uint8_t version = kProtocolVersion;   // 4 bit
    uint8_t seq_len = 4;                  // 4 bit (1~8)
    MsgType msg_type = MsgType::DATA;
    SeqNum  seq_num = 0;

    // Serialized size depends on seq_len
    size_t serializedSize() const { return 2 + seq_len; }

    // Serialize to buffer. Returns bytes written, or 0 on error.
    size_t serialize(uint8_t* buf, size_t buf_len) const;

    // Deserialize from buffer. Returns true on success.
    static bool deserialize(const uint8_t* buf, size_t buf_len,
                            CommonHeader& out, size_t& bytes_consumed);
};

// DATA packet header (follows CommonHeader)
struct DataHeader {
    uint16_t frame_id = 0;                  // Frame identifier (unique per frame, wraps at 65535)
    uint8_t  data_block_count = 0;
    uint8_t  fec_block_count = 0;
    uint8_t  block_index = 0;
    uint8_t  fec_group_index = 0;
    uint8_t  fec_group_count = 0;
    FrameType frame_type = 0;
    uint32_t frame_size = 0;
    uint8_t  retry_count = 0;             // 4 bit (0~15)
    uint16_t payload_length = 0;          // 12 bit (0~4095)

    static constexpr size_t serializedSize() { return 14; }

    size_t serialize(uint8_t* buf, size_t buf_len) const;
    static bool deserialize(const uint8_t* buf, size_t buf_len, DataHeader& out);
};

// NACK payload serialization
size_t serializeNackPayload(const std::vector<NackEntry>& entries,
                            uint8_t* buf, size_t buf_len, uint8_t seq_len);
bool deserializeNackPayload(const uint8_t* buf, size_t payload_len,
                            std::vector<NackEntry>& out);

// TLV payload serialization
size_t serializeTlvPayload(const std::vector<TlvItem>& items,
                           uint8_t* buf, size_t buf_len);
bool deserializeTlvPayload(const uint8_t* buf, size_t payload_len,
                           std::vector<TlvItem>& out);

// Compute the minimum seq_len (bytes) needed to represent a sequence number.
// seq == 0 → 0, seq <= 0xFF → 1, seq <= 0xFFFF → 2, ..., up to 8.
uint8_t computeSeqLen(SeqNum seq);

// Sequence number helpers
SeqNum seqMask(uint8_t seq_len);
int seqCompare(SeqNum a, SeqNum b, uint8_t seq_len);
SeqNum seqAdd(SeqNum a, uint64_t offset, uint8_t seq_len);

// Big-endian read/write helpers
void writeBE16(uint8_t* buf, uint16_t val);
void writeBE32(uint8_t* buf, uint32_t val);
void writeBE64(uint8_t* buf, uint64_t val);
uint16_t readBE16(const uint8_t* buf);
uint32_t readBE32(const uint8_t* buf);
uint64_t readBE64(const uint8_t* buf);

// Write sequence number in big-endian with given byte length
void writeSeqNum(uint8_t* buf, SeqNum val, uint8_t seq_len);
SeqNum readSeqNum(const uint8_t* buf, uint8_t seq_len);

} // namespace fec_protocol

