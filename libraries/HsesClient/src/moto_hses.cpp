/*
MIT License
Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

NetBurner port of MotoHSES moto_hses.cpp.
Derived from that source; Winsock/BSD I/O replaced with NetBurner UDP
(CreateRxUdpSocket / sendto4 / recvfrom4 / select). Heap vectors replaced with
fixed stack buffers.
*/

#include <HsesClient/moto_hses.h>

#include <cstdio>
#include <cstring>
#include <stdio.h> // sniprintf

#include <iosys.h>
#include <nbrtos.h>
#include <udp.h>

#if !defined(MOTO_HSES_UDP_MAX_ATTEMPTS)
#define MOTO_HSES_UDP_MAX_ATTEMPTS_INTERNAL 1
#elif (MOTO_HSES_UDP_MAX_ATTEMPTS) < 1
#define MOTO_HSES_UDP_MAX_ATTEMPTS_INTERNAL 1
#else
#define MOTO_HSES_UDP_MAX_ATTEMPTS_INTERNAL (MOTO_HSES_UDP_MAX_ATTEMPTS)
#endif

namespace moto {
namespace hses {

namespace {

void set_error(char *dst, size_t cap, const char *msg)
{
    if (!dst || cap < 2) {
        return;
    }
    if (!msg) {
        dst[0] = '\0';
        return;
    }
    const size_t n = strlen(msg);
    const size_t c = (n < cap - 1) ? n : (cap - 1);
    memcpy(dst, msg, c);
    dst[c] = '\0';
}

uint16_t read_u16_le(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32_le(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void write_u16_le(uint8_t *out, size_t idx, uint16_t v)
{
    out[idx] = static_cast<uint8_t>(v & 0xFF);
    out[idx + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void write_u32_le(uint8_t *out, size_t idx, uint32_t v)
{
    out[idx] = static_cast<uint8_t>(v & 0xFF);
    out[idx + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[idx + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[idx + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void append_u32_le(uint8_t *buf, size_t &len, size_t cap, uint32_t v)
{
    if (len + 4 > cap) {
        return;
    }
    write_u32_le(buf, len, v);
    len += 4;
}

void append_i32_le(uint8_t *buf, size_t &len, size_t cap, int32_t v)
{
    append_u32_le(buf, len, cap, static_cast<uint32_t>(v));
}

int32_t round_to_i32(double v)
{
    return static_cast<int32_t>(v >= 0.0 ? (v + 0.5) : (v - 0.5));
}

uint32_t timeout_to_ticks(int timeout_ms)
{
    if (timeout_ms <= 0) {
        return TICKS_PER_SECOND * 2;
    }
    const uint32_t ticks = static_cast<uint32_t>((static_cast<uint64_t>(timeout_ms) * TICKS_PER_SECOND) / 1000u);
    return ticks == 0 ? 1u : ticks;
}

void encode_position_variable(uint8_t *payload, size_t &len, size_t cap, const PositionVariableData &data)
{
    len = 0;
    append_u32_le(payload, len, cap, data.data_type);
    append_u32_le(payload, len, cap, 0);
    append_u32_le(payload, len, cap, 0);
    append_u32_le(payload, len, cap, 0);
    append_u32_le(payload, len, cap, 0);
    append_i32_le(payload, len, cap, data.x);
    append_i32_le(payload, len, cap, data.y);
    append_i32_le(payload, len, cap, data.z);
    append_i32_le(payload, len, cap, data.rx);
    append_i32_le(payload, len, cap, data.ry);
    append_i32_le(payload, len, cap, data.rz);
    append_i32_le(payload, len, cap, 0);
    append_i32_le(payload, len, cap, 0);
}

} // namespace

namespace decode {

bool positionRead(const uint8_t *payload, size_t payloadLen, PositionReadData &out)
{
    out = PositionReadData{};
    constexpr size_t kHdr = 20;
    if (!payload || payloadLen < kHdr + 6 * 4) {
        return false;
    }
    out.data_type = static_cast<int32_t>(read_u32_le(payload));
    out.form = static_cast<int32_t>(read_u32_le(payload + 4));
    out.tool_number = static_cast<int32_t>(read_u32_le(payload + 8));
    out.user_coordinate_number = static_cast<int32_t>(read_u32_le(payload + 12));
    out.extended_form = static_cast<int32_t>(read_u32_le(payload + 16));
    const size_t axis_bytes = payloadLen - kHdr;
    if ((axis_bytes % 4) != 0) {
        return false;
    }
    const size_t axis_count = axis_bytes / 4;
    if (axis_count < 6 || axis_count > 8) {
        return false;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (i < axis_count) {
            out.axis_data[i] = static_cast<int32_t>(read_u32_le(payload + 20 + i * 4));
        }
    }
    out.valid = true;
    return true;
}

} // namespace decode

void MoveCartesianData::setXYZ_mm(double x_mm, double y_mm, double z_mm)
{
    x_um = round_to_i32(x_mm * 1000.0);
    y_um = round_to_i32(y_mm * 1000.0);
    z_um = round_to_i32(z_mm * 1000.0);
}

void MoveCartesianData::setTxTyTz_deg(double tx_deg, double ty_deg, double tz_deg)
{
    tx_0p0001deg = round_to_i32(tx_deg * 10000.0);
    ty_0p0001deg = round_to_i32(ty_deg * 10000.0);
    tz_0p0001deg = round_to_i32(tz_deg * 10000.0);
}

size_t MoveCartesianData::serialize(uint8_t *out, size_t outCap) const
{
    if (!out || outCap < 26 * 4) {
        return 0;
    }
    size_t len = 0;
    append_u32_le(out, len, outCap, control_group_robot);
    append_u32_le(out, len, outCap, control_group_station);
    append_u32_le(out, len, outCap, static_cast<uint32_t>(speed_class));
    append_i32_le(out, len, outCap, speed);
    append_u32_le(out, len, outCap, static_cast<uint32_t>(coordinate));
    append_i32_le(out, len, outCap, x_um);
    append_i32_le(out, len, outCap, y_um);
    append_i32_le(out, len, outCap, z_um);
    append_i32_le(out, len, outCap, tx_0p0001deg);
    append_i32_le(out, len, outCap, ty_0p0001deg);
    append_i32_le(out, len, outCap, tz_0p0001deg);
    append_u32_le(out, len, outCap, reservation_1);
    append_u32_le(out, len, outCap, reservation_2);
    append_u32_le(out, len, outCap, type);
    append_u32_le(out, len, outCap, expanded_type);
    append_u32_le(out, len, outCap, tool_no);
    append_u32_le(out, len, outCap, user_coordinate_no);
    append_i32_le(out, len, outCap, base_axis_um[0]);
    append_i32_le(out, len, outCap, base_axis_um[1]);
    append_i32_le(out, len, outCap, base_axis_um[2]);
    for (int i = 0; i < 6; ++i) {
        append_i32_le(out, len, outCap, station_axis_pulse[i]);
    }
    return len;
}

size_t MovePulseData::serialize(uint8_t *out, size_t outCap) const
{
    const size_t need = (4 + 8 + 1 + 3 + 6) * 4;
    if (!out || outCap < need) {
        return 0;
    }
    size_t len = 0;
    append_u32_le(out, len, outCap, control_group_robot);
    append_u32_le(out, len, outCap, control_group_station);
    append_u32_le(out, len, outCap, static_cast<uint32_t>(speed_class));
    append_i32_le(out, len, outCap, speed);
    for (int i = 0; i < 8; ++i) {
        append_i32_le(out, len, outCap, robot_axis_pulse[i]);
    }
    append_u32_le(out, len, outCap, tool_no);
    for (int i = 0; i < 3; ++i) {
        append_i32_le(out, len, outCap, base_axis_pulse[i]);
    }
    for (int i = 0; i < 6; ++i) {
        append_i32_le(out, len, outCap, station_axis_pulse[i]);
    }
    return len;
}

struct Client::Impl {
    int sock = -1;
    IPADDR4 peer_ip{};
    uint16_t peer_port = kDefaultControlPort;
    uint16_t file_udp_port = kDefaultFilePort;
    bool open = false;
    ControllerProfile profile = ControllerProfile::DX200;
    uint16_t learned_char32_command = 0;
};

Client::Client() : impl_(new Impl()) {}

Client::~Client()
{
    close();
    delete impl_;
    impl_ = nullptr;
}

bool Client::open(const Endpoint &endpoint, uint16_t local_port, char *error, size_t errorCap)
{
    close();

    if (endpoint.ip.IsNull()) {
        set_error(error, errorCap, "Endpoint IP is null.");
        return false;
    }

    const uint16_t bindPort = (local_port == 0) ? kDefaultLocalUdpPort : local_port;
    impl_->sock = CreateRxUdpSocket(bindPort);
    if (impl_->sock < 0) {
        set_error(error, errorCap, "Failed to create HSES UDP socket.");
        return false;
    }

    impl_->peer_ip = endpoint.ip;
    impl_->peer_port = (endpoint.port == 0) ? kDefaultControlPort : endpoint.port;
    if (endpoint.file_port != 0) {
        impl_->file_udp_port = endpoint.file_port;
    } else if (impl_->peer_port == kDefaultControlPort) {
        impl_->file_udp_port = kDefaultFilePort;
    } else {
        impl_->file_udp_port = impl_->peer_port;
    }

    impl_->open = true;
    return true;
}

void Client::close()
{
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->sock >= 0) {
        ::close(impl_->sock);
        impl_->sock = -1;
    }
    impl_->open = false;
    impl_->peer_ip = IPADDR4{};
}

bool Client::is_open() const
{
    return impl_ != nullptr && impl_->open;
}

void Client::setControllerProfile(ControllerProfile profile)
{
    if (impl_ != nullptr) {
        impl_->profile = profile;
        impl_->learned_char32_command = 0;
    }
}

ControllerProfile Client::controllerProfile() const
{
    if (impl_ == nullptr) {
        return ControllerProfile::DX200;
    }
    return impl_->profile;
}

Command Client::activeChar32Command() const
{
    if (impl_ != nullptr && impl_->learned_char32_command != 0) {
        return static_cast<Command>(impl_->learned_char32_command);
    }
    return (controllerProfile() == ControllerProfile::DX200) ? Command::Char32VarReadWrite
                                                             : Command::Char32VarReadWriteLegacy;
}

bool Client::isUndefinedCommandResponse(const Response &response) const
{
    if (response.transport_ok && response.status == 0x08) {
        return true;
    }
    if (response.transport_ok && response.added_status == 0xA000) {
        return true;
    }
    return false;
}

Response Client::char32RequestWithFallback(uint16_t index,
                                           Service service,
                                           const uint8_t *payload,
                                           size_t payloadLen,
                                           uint8_t request_id,
                                           int timeout_ms)
{
    const Command primary = activeChar32Command();
    Response first = request(primary, index, 0x00, service, payload, payloadLen, request_id, timeout_ms);
    if (first.ok() || !isUndefinedCommandResponse(first)) {
        if (first.ok() && impl_ != nullptr) {
            impl_->learned_char32_command = static_cast<uint16_t>(primary);
        }
        return first;
    }

    const Command alternate = (primary == Command::Char32VarReadWrite) ? Command::Char32VarReadWriteLegacy
                                                                      : Command::Char32VarReadWrite;
    Response second = request(alternate, index, 0x00, service, payload, payloadLen, request_id, timeout_ms);
    if (second.ok() && impl_ != nullptr) {
        impl_->learned_char32_command = static_cast<uint16_t>(alternate);
    }
    return second;
}

Response Client::request(Command command,
                         uint16_t instance,
                         uint8_t attribute,
                         Service service,
                         const uint8_t *payload,
                         size_t payloadLen,
                         const RequestOptions &options)
{
    Response out;
    out.command_no = static_cast<uint16_t>(command);
    out.instance = instance;
    out.attribute = attribute;
    out.request_id = options.request_id;
    out.block_no = options.block_no;

    if (!is_open()) {
        set_error(out.error, sizeof(out.error), "Socket is not open.");
        return out;
    }
    if (payloadLen > kMaxPayloadSize || (payloadLen > 0 && payload == nullptr)) {
        set_error(out.error, sizeof(out.error), "Invalid payload.");
        return out;
    }

    uint8_t frame[kMaxPacketSize]{};
    const size_t frameLen = kHeaderSize + payloadLen;
    frame[0] = 'Y';
    frame[1] = 'E';
    frame[2] = 'R';
    frame[3] = 'C';
    write_u16_le(frame, 4, static_cast<uint16_t>(kHeaderSize));
    write_u16_le(frame, 6, static_cast<uint16_t>(payloadLen));
    frame[8] = 0x03;
    frame[9] = options.processing_division;
    frame[10] = options.ack;
    frame[11] = options.request_id;
    write_u32_le(frame, 12, options.block_no);
    for (size_t i = 16; i < 24; ++i) {
        frame[i] = '9';
    }
    write_u16_le(frame, 24, static_cast<uint16_t>(command));
    write_u16_le(frame, 26, instance);
    frame[28] = attribute;
    frame[29] = static_cast<uint8_t>(service);
    write_u16_le(frame, 30, 0x0000);
    if (payloadLen > 0) {
        memcpy(frame + kHeaderSize, payload, payloadLen);
    }

    const uint16_t destPort =
        (options.processing_division == 2) ? impl_->file_udp_port : impl_->peer_port;
    const uint32_t timeoutTicks = timeout_to_ticks(options.timeout_ms);

    int rx_len = 0;
    uint8_t rx[kMaxPacketSize]{};

    for (int attempt = 0; attempt < MOTO_HSES_UDP_MAX_ATTEMPTS_INTERNAL; ++attempt) {
        const int sent =
            sendto4(impl_->sock, frame, static_cast<int>(frameLen), impl_->peer_ip, destPort);
        if (sent != static_cast<int>(frameLen)) {
            set_error(out.error, sizeof(out.error), "sendto() failed.");
            return out;
        }

        const uint32_t start = TimeTick;
        bool got = false;
        while ((TimeTick - start) < timeoutTicks) {
            const uint32_t elapsed = TimeTick - start;
            const uint32_t remaining = (elapsed < timeoutTicks) ? (timeoutTicks - elapsed) : 0;
            if (remaining == 0) {
                break;
            }

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(impl_->sock, &readfds);
            const uint32_t wait = (remaining < TICKS_PER_SECOND) ? remaining : TICKS_PER_SECOND;
            const int ready = select(impl_->sock + 1, &readfds, nullptr, nullptr, wait);
            if (ready <= 0) {
                continue;
            }

            IPADDR4 fromIp{};
            uint16_t localPort = 0;
            uint16_t remotePort = 0;
            rx_len = recvfrom4(impl_->sock, rx, sizeof(rx), &fromIp, &localPort, &remotePort);
            if (rx_len < static_cast<int>(kHeaderSize)) {
                continue;
            }
            got = true;
            break;
        }

        if (got) {
            break;
        }
        if (attempt + 1 >= MOTO_HSES_UDP_MAX_ATTEMPTS_INTERNAL) {
            set_error(out.error, sizeof(out.error), "Timeout waiting for response.");
            return out;
        }
    }

    if (rx_len < static_cast<int>(kHeaderSize)) {
        set_error(out.error, sizeof(out.error), "Received packet is too short.");
        return out;
    }
    if (!(rx[0] == 'Y' && rx[1] == 'E' && rx[2] == 'R' && rx[3] == 'C')) {
        set_error(out.error, sizeof(out.error), "Received packet does not have YERC signature.");
        return out;
    }

    out.transport_ok = true;
    out.block_no = read_u32_le(&rx[12]);
    out.request_id = rx[11];
    out.service = rx[24];
    out.status = rx[25];
    out.added_status_size = rx[26];
    out.added_status = read_u16_le(&rx[28]);

    const uint16_t data_size = read_u16_le(&rx[6]);
    int payload_bytes = static_cast<int>(data_size);
    const int avail = rx_len - static_cast<int>(kHeaderSize);
    if (payload_bytes > avail) {
        payload_bytes = avail;
    }
    if (payload_bytes > static_cast<int>(kMaxPayloadSize)) {
        payload_bytes = static_cast<int>(kMaxPayloadSize);
    }
    if (payload_bytes > 0) {
        memcpy(out.payload, rx + kHeaderSize, static_cast<size_t>(payload_bytes));
        out.payload_len = static_cast<size_t>(payload_bytes);
    }
    return out;
}

Response Client::request(Command command,
                         uint16_t instance,
                         uint8_t attribute,
                         Service service,
                         const uint8_t *payload,
                         size_t payloadLen,
                         uint8_t request_id,
                         int timeout_ms)
{
    RequestOptions options;
    options.request_id = request_id;
    options.timeout_ms = timeout_ms;
    return request(command, instance, attribute, service, payload, payloadLen, options);
}

Response Client::readCommand(Command command,
                             uint16_t instance,
                             uint8_t attribute,
                             bool read_all,
                             const RequestOptions &options)
{
    return request(command,
                   instance,
                   attribute,
                   read_all ? Service::GetAttributeAll : Service::GetAttributeSingle,
                   nullptr,
                   0,
                   options);
}

Response Client::writeCommand(Command command,
                              uint16_t instance,
                              uint8_t attribute,
                              const uint8_t *payload,
                              size_t payloadLen,
                              bool write_single,
                              const RequestOptions &options)
{
    return request(command,
                   instance,
                   attribute,
                   write_single ? Service::SetAttributeSingle : Service::SetAttributeAll,
                   payload,
                   payloadLen,
                   options);
}

Response Client::readStatusAll(uint8_t request_id, int timeout_ms)
{
    return request(Command::StatusRead, 0x0001, 0x00, Service::GetAttributeAll, nullptr, 0, request_id, timeout_ms);
}

Response Client::readAlarmData(uint16_t instance, uint8_t attribute, bool read_all, uint8_t request_id, int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::AlarmRead, instance, attribute, read_all, o);
}

Response Client::readAlarmHistory(uint16_t instance, uint8_t attribute, bool read_all, uint8_t request_id, int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::AlarmHistoryRead, instance, attribute, read_all, o);
}

Response Client::readExecutingJobInfo(uint16_t task_instance,
                                      uint8_t attribute,
                                      bool read_all,
                                      uint8_t request_id,
                                      int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::ExecutingJobRead, task_instance, attribute, read_all, o);
}

Response Client::readAxisConfiguration(uint16_t instance,
                                       uint8_t attribute,
                                       bool read_all,
                                       uint8_t request_id,
                                       int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::AxisConfigRead, instance, attribute, read_all, o);
}

Response Client::readPositionCartesian(uint8_t request_id, int timeout_ms, uint16_t instance)
{
    return request(Command::PositionRead, instance, 0x00, Service::GetAttributeAll, nullptr, 0, request_id, timeout_ms);
}

Response Client::readPositionPulse(uint8_t request_id, int timeout_ms, uint16_t instance)
{
    return request(Command::PositionRead, instance, 0x00, Service::GetAttributeAll, nullptr, 0, request_id, timeout_ms);
}

Response Client::readPositionError(uint16_t instance, uint8_t attribute, bool read_all, uint8_t request_id, int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::PositionErrorRead, instance, attribute, read_all, o);
}

Response Client::readTorqueData(uint16_t instance, uint8_t attribute, bool read_all, uint8_t request_id, int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::TorqueRead, instance, attribute, read_all, o);
}

Response Client::readEncoderTemperature(uint16_t instance,
                                        uint8_t attribute,
                                        bool read_all,
                                        uint8_t request_id,
                                        int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::EncoderTemperatureRead, instance, attribute, read_all, o);
}

Response Client::readConverterTemperature(uint16_t instance,
                                          uint8_t attribute,
                                          bool read_all,
                                          uint8_t request_id,
                                          int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::ConverterTemperatureRead, instance, attribute, read_all, o);
}

Response Client::servoOn(uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), 1);
    return request(Command::HoldServo, 0x0002, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::servoOff(uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), 2);
    return request(Command::HoldServo, 0x0002, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::holdOn(uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), 1);
    return request(Command::HoldServo, 0x0001, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::holdOff(uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), 2);
    return request(Command::HoldServo, 0x0001, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::setHoldLock(bool enable, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), enable ? 1u : 2u);
    return request(Command::HoldServo, 0x0003, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::setStepCycleContinuous(uint32_t mode, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), mode);
    return request(
        Command::StepCycleContinuous, 0x0001, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::displayPendantString(const char *text, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[32]{};
    if (text) {
        const size_t n = strlen(text);
        memcpy(payload, text, (n < sizeof(payload)) ? n : sizeof(payload));
    }
    return request(
        Command::PendantStringDisplay, 0x0001, 0x01, Service::SetAttributeAll, payload, sizeof(payload), request_id, timeout_ms);
}

Response Client::readManagementTime(uint8_t attribute,
                                    bool read_all,
                                    uint8_t request_id,
                                    int timeout_ms,
                                    uint16_t instance)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::ManagementTimeRead, instance, attribute, read_all, o);
}

Response Client::readSystemInformation(uint8_t attribute,
                                       bool read_all,
                                       uint8_t request_id,
                                       int timeout_ms,
                                       uint16_t instance)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return readCommand(Command::SystemInfoRead, instance, attribute, read_all, o);
}

Response Client::alarmResetOrErrorCancel(uint32_t mode, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), mode);
    return request(
        Command::AlarmResetErrorCancel, 0x0001, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::readIo(uint16_t io_address, uint8_t request_id, int timeout_ms)
{
    return request(Command::IoReadWrite, io_address, 0x01, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeOutput(uint16_t io_address, uint8_t value, uint8_t request_id, int timeout_ms)
{
    // DX200 implementations often use service 0x0E for single I/O element write.
    return request(Command::IoReadWrite, io_address, 0x01, Service::GetAttributeSingle, &value, 1, request_id, timeout_ms);
}

Response Client::readRegister(uint16_t reg, uint8_t request_id, int timeout_ms)
{
    return request(Command::RegisterReadWrite, reg, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeRegister(uint16_t reg, uint32_t value, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), value);
    return request(
        Command::RegisterReadWrite, reg, 0x00, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::readByteVariable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(Command::ByteVarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeByteVariable(uint16_t index, uint8_t value, uint8_t request_id, int timeout_ms)
{
    return request(Command::ByteVarReadWrite, index, 0x00, Service::SetAttributeSingle, &value, 1, request_id, timeout_ms);
}

Response Client::readIntVariable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(Command::IntVarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeIntVariable(uint16_t index, int16_t value, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
    return request(Command::IntVarReadWrite, index, 0x00, Service::SetAttributeSingle, payload, 2, request_id, timeout_ms);
}

Response Client::readDintVariable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(Command::DintVarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeDintVariable(uint16_t index, int32_t value, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_i32_le(payload, len, sizeof(payload), value);
    return request(
        Command::DintVarReadWrite, index, 0x00, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::readRealVariable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(Command::RealVarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeRealVariable(uint16_t index, float value, uint8_t request_id, int timeout_ms)
{
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), bits);
    return request(
        Command::RealVarReadWrite, index, 0x00, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::readChar16Variable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(Command::Char16VarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeChar16Variable(uint16_t index, const char *value, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[16]{};
    if (value) {
        const size_t n = strlen(value);
        memcpy(payload, value, (n < sizeof(payload)) ? n : sizeof(payload));
    }
    return request(
        Command::Char16VarReadWrite, index, 0x00, Service::SetAttributeSingle, payload, sizeof(payload), request_id, timeout_ms);
}

Response Client::readChar32Variable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return char32RequestWithFallback(index, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeChar32Variable(uint16_t index, const char *value, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[32]{};
    if (value) {
        const size_t n = strlen(value);
        memcpy(payload, value, (n < sizeof(payload)) ? n : sizeof(payload));
    }
    return char32RequestWithFallback(index, Service::SetAttributeSingle, payload, sizeof(payload), request_id, timeout_ms);
}

Response Client::readPositionVariable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(
        Command::PositionVarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writePositionVariable(uint16_t index,
                                       const PositionVariableData &data,
                                       uint8_t request_id,
                                       int timeout_ms)
{
    uint8_t payload[52]{};
    size_t len = 0;
    encode_position_variable(payload, len, sizeof(payload), data);
    return request(
        Command::PositionVarReadWrite, index, 0x00, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

Response Client::readBasePositionVariable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(
        Command::BasePositionVarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeBasePositionVariable(uint16_t index,
                                           const PositionVariableData &data,
                                           uint8_t request_id,
                                           int timeout_ms)
{
    uint8_t payload[52]{};
    size_t len = 0;
    encode_position_variable(payload, len, sizeof(payload), data);
    return request(Command::BasePositionVarReadWrite,
                   index,
                   0x00,
                   Service::SetAttributeSingle,
                   payload,
                   len,
                   request_id,
                   timeout_ms);
}

Response Client::readExternalAxisVariable(uint16_t index, uint8_t request_id, int timeout_ms)
{
    return request(
        Command::ExternalAxisVarReadWrite, index, 0x00, Service::GetAttributeSingle, nullptr, 0, request_id, timeout_ms);
}

Response Client::writeExternalAxisVariable(uint16_t index,
                                           const PositionVariableData &data,
                                           uint8_t request_id,
                                           int timeout_ms)
{
    uint8_t payload[52]{};
    size_t len = 0;
    encode_position_variable(payload, len, sizeof(payload), data);
    return request(Command::ExternalAxisVarReadWrite,
                   index,
                   0x00,
                   Service::SetAttributeSingle,
                   payload,
                   len,
                   request_id,
                   timeout_ms);
}

Response Client::moveCartesianRaw(const uint8_t *payload, size_t payloadLen, uint8_t request_id, int timeout_ms)
{
    return request(Command::MoveCartesian, 0x0001, 0x01, Service::SetAttributeAll, payload, payloadLen, request_id, timeout_ms);
}

Response Client::movePulseRaw(const uint8_t *payload, size_t payloadLen, uint8_t request_id, int timeout_ms)
{
    return request(Command::MovePulse, 0x0001, 0x01, Service::SetAttributeAll, payload, payloadLen, request_id, timeout_ms);
}

Response Client::moveCartesian(const MoveCartesianData &data,
                               MoveCartesianOperation op,
                               uint8_t request_id,
                               int timeout_ms)
{
    Response r;
    r.command_no = static_cast<uint16_t>(Command::MoveCartesian);
    r.instance = static_cast<uint16_t>(op);
    r.attribute = 0x01;
    if (data.control_group_robot != 0 && data.control_group_station != 0) {
        set_error(r.error,
                  sizeof(r.error),
                  "Invalid control group selection: cannot operate robot and station simultaneously for move commands.");
        return r;
    }
    if (data.tool_no > 63) {
        set_error(r.error, sizeof(r.error), "Invalid tool_no: must be 0..63.");
        return r;
    }
    if (data.coordinate == MoveCoordinate::User) {
        if (data.user_coordinate_no < 1 || data.user_coordinate_no > 63) {
            set_error(r.error, sizeof(r.error), "Invalid user_coordinate_no for User coordinate: must be 1..63.");
            return r;
        }
    }
    uint8_t payload[26 * 4]{};
    const size_t len = data.serialize(payload, sizeof(payload));
    return request(Command::MoveCartesian,
                   static_cast<uint16_t>(op),
                   0x01,
                   Service::SetAttributeAll,
                   payload,
                   len,
                   request_id,
                   timeout_ms);
}

Response Client::movePulse(const MovePulseData &data, MovePulseOperation op, uint8_t request_id, int timeout_ms)
{
    Response r;
    r.command_no = static_cast<uint16_t>(Command::MovePulse);
    r.instance = static_cast<uint16_t>(op);
    r.attribute = 0x01;
    if (data.control_group_robot != 0 && data.control_group_station != 0) {
        set_error(r.error,
                  sizeof(r.error),
                  "Invalid control group selection: cannot operate robot and station simultaneously for move commands.");
        return r;
    }
    if (data.tool_no > 63) {
        set_error(r.error, sizeof(r.error), "Invalid tool_no: must be 0..63.");
        return r;
    }
    uint8_t payload[88]{};
    const size_t len = data.serialize(payload, sizeof(payload));
    return request(
        Command::MovePulse, static_cast<uint16_t>(op), 0x01, Service::SetAttributeAll, payload, len, request_id, timeout_ms);
}

Response Client::multiIoReadWrite(uint16_t instance,
                                  uint8_t attribute,
                                  const uint8_t *payload,
                                  size_t payloadLen,
                                  bool write_single,
                                  uint8_t request_id,
                                  int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiIoReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiRegisterReadWrite(uint16_t instance,
                                        uint8_t attribute,
                                        const uint8_t *payload,
                                        size_t payloadLen,
                                        bool write_single,
                                        uint8_t request_id,
                                        int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiRegisterReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiByteVarReadWrite(uint16_t instance,
                                       uint8_t attribute,
                                       const uint8_t *payload,
                                       size_t payloadLen,
                                       bool write_single,
                                       uint8_t request_id,
                                       int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiByteVarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiIntVarReadWrite(uint16_t instance,
                                      uint8_t attribute,
                                      const uint8_t *payload,
                                      size_t payloadLen,
                                      bool write_single,
                                      uint8_t request_id,
                                      int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiIntVarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiDintVarReadWrite(uint16_t instance,
                                       uint8_t attribute,
                                       const uint8_t *payload,
                                       size_t payloadLen,
                                       bool write_single,
                                       uint8_t request_id,
                                       int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiDintVarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiRealVarReadWrite(uint16_t instance,
                                       uint8_t attribute,
                                       const uint8_t *payload,
                                       size_t payloadLen,
                                       bool write_single,
                                       uint8_t request_id,
                                       int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiRealVarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiChar16VarReadWrite(uint16_t instance,
                                         uint8_t attribute,
                                         const uint8_t *payload,
                                         size_t payloadLen,
                                         bool write_single,
                                         uint8_t request_id,
                                         int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiChar16VarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiPositionVarReadWrite(uint16_t instance,
                                           uint8_t attribute,
                                           const uint8_t *payload,
                                           size_t payloadLen,
                                           bool write_single,
                                           uint8_t request_id,
                                           int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiPositionVarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiBasePositionVarReadWrite(uint16_t instance,
                                               uint8_t attribute,
                                               const uint8_t *payload,
                                               size_t payloadLen,
                                               bool write_single,
                                               uint8_t request_id,
                                               int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(
        Command::MultiBasePositionVarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiExternalAxisVarReadWrite(uint16_t instance,
                                               uint8_t attribute,
                                               const uint8_t *payload,
                                               size_t payloadLen,
                                               bool write_single,
                                               uint8_t request_id,
                                               int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(
        Command::MultiExternalAxisVarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::multiChar32VarReadWrite(uint16_t instance,
                                         uint8_t attribute,
                                         const uint8_t *payload,
                                         size_t payloadLen,
                                         bool write_single,
                                         uint8_t request_id,
                                         int timeout_ms)
{
    RequestOptions o;
    o.request_id = request_id;
    o.timeout_ms = timeout_ms;
    return writeCommand(Command::MultiChar32VarReadWrite, instance, attribute, payload, payloadLen, write_single, o);
}

Response Client::selectJob(const char *job_name, uint32_t line_number, uint8_t request_id, int timeout_ms)
{
    uint8_t payload[36]{};
    if (job_name) {
        const size_t n = strlen(job_name);
        memcpy(payload, job_name, (n < 32) ? n : 32);
    }
    write_u32_le(payload, 32, line_number);
    return request(Command::JobSelect, 0x0001, 0x01, Service::SetAttributeAll, payload, sizeof(payload), request_id, timeout_ms);
}

Response Client::startJob(uint8_t request_id, int timeout_ms)
{
    uint8_t payload[4]{};
    size_t len = 0;
    append_u32_le(payload, len, sizeof(payload), 1);
    return request(Command::JobStart, 0x0001, 0x01, Service::SetAttributeSingle, payload, len, request_id, timeout_ms);
}

StatusFlags Client::decodeStatus(const Response &response)
{
    StatusFlags s;
    if (response.payload_len < 8) {
        return s;
    }
    s.data1 = read_u32_le(response.payload);
    s.data2 = read_u32_le(response.payload + 4);
    s.step_mode = (s.data1 & (1u << 0)) != 0;
    s.one_cycle_mode = (s.data1 & (1u << 1)) != 0;
    s.auto_continuous_mode = (s.data1 & (1u << 2)) != 0;
    s.running = (s.data1 & (1u << 3)) != 0;
    s.in_guard_safe = (s.data1 & (1u << 4)) != 0;
    s.teach_mode = (s.data1 & (1u << 5)) != 0;
    s.play_mode = (s.data1 & (1u << 6)) != 0;
    s.command_remote = (s.data1 & (1u << 7)) != 0;
    s.hold_pendant = (s.data2 & (1u << 1)) != 0;
    s.hold_external = (s.data2 & (1u << 2)) != 0;
    s.hold_command = (s.data2 & (1u << 3)) != 0;
    s.alarm = (s.data2 & (1u << 4)) != 0;
    s.error = (s.data2 & (1u << 5)) != 0;
    s.servo_on = (s.data2 & (1u << 6)) != 0;
    return s;
}

void Client::statusSummary(const StatusFlags &s, char *out, size_t outCap)
{
    if (!out || outCap < 2) {
        return;
    }
    sniprintf(out,
              outCap,
              "Servo=%s Running=%s Mode=%s Remote=%s Alarm=%s Error=%s Hold=[%c%c%c] D1=0x%08lX D2=0x%08lX",
              s.servo_on ? "ON" : "OFF",
              s.running ? "YES" : "NO",
              s.teach_mode ? "TEACH" : (s.play_mode ? "PLAY" : "UNKNOWN"),
              s.command_remote ? "ON" : "OFF",
              s.alarm ? "ACTIVE" : "OK",
              s.error ? "ACTIVE" : "OK",
              s.hold_pendant ? 'P' : '-',
              s.hold_external ? 'E' : '-',
              s.hold_command ? 'C' : '-',
              static_cast<unsigned long>(s.data1),
              static_cast<unsigned long>(s.data2));
}

const char *Client::explainStatus(uint8_t status)
{
    switch (status) {
    case 0x08:
        return "Requested command is not defined";
    case 0x09:
        return "Check request subheader values";
    case 0x28:
        return "Invalid instance (instance field in request header is not accepted for this command)";
    default:
        return nullptr;
    }
}

const char *Client::explainAddedStatus(uint16_t added_status)
{
    switch (added_status) {
    case 2010:
        return "Manipulator operating";
    case 2020:
        return "Hold by programming pendant";
    case 2030:
        return "Hold by playback panel";
    case 2040:
        return "External hold";
    case 2050:
        return "Command hold";
    case 2060:
        return "Error/alarm occurring";
    case 2070:
        return "Servo OFF";
    case 2080:
        return "Incorrect mode";
    case 2090:
        return "File accessing by other function";
    case 2100:
        return "Command remote not set";
    case 3010:
        return "Turn ON the servo power";
    case 3040:
        return "Perform home positioning";
    case 3050:
        return "Confirm positions";
    case 3070:
        return "Current value not made";
    case 3220:
        return "Panel lock; mode/cycle prohibit signal is ON";
    case 3230:
        return "Panel lock; start prohibit signal is ON";
    case 3350:
        return "User coordinate is not taught";
    case 3360:
        return "User coordinate is destroyed";
    case 3370:
        return "Incorrect control group";
    case 3380:
        return "Incorrect base axis data";
    case 0xA000:
        return "Undefined command";
    case 0xB008:
        return "Incorrect control group setting";
    case 0xE4A3:
        return "Format error (processing category); file ops may need UDP port 10041";
    case 0xE28C:
        return "Cannot overwrite the target file (enable HSES overwrite on controller or delete job first)";
    case 0xFFFE:
        return "Job upload (FileLoad) rejected — controller-specific; check command remote, remote PLAY, Ethernet file overwrite RS029/RS214 if replacing a job, and your HSES manual";
    default:
        return nullptr;
    }
}

} // namespace hses
} // namespace moto
