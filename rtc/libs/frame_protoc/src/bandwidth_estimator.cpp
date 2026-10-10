#include "bandwidth_estimator.h"

namespace fec_protocol {

uint32_t estimateBandwidth(const std::vector<BwSample>& samples) {
    // TODO: 实现带宽估计算法。可用信息：
    //   - samples[i].send_time_us  发送端向网络发包时刻（同一时钟域内可比）
    //   - samples[i].recv_time_ms  接收端收到时刻（同一时钟域内可比，与发送端无共同基准）
    //   - samples[i].packet_size   整包字节数
    //   典型思路：批内发送间隔 vs 接收间隔的梯度判断排队趋势，
    //   结合接收窗口内 sum(packet_size)*8 / 接收时间跨度 得到到达速率
    (void)samples;
    return 0;
}

} // namespace fec_protocol
