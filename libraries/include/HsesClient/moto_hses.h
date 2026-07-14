/*
MIT License

Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

NetBurner port of MotoHSES (moto_hses.h / moto_hses.cpp).
Protocol behavior and API shape are derived from that project. Transport uses
NetBurner UDP instead of Winsock/BSD sockets. std::vector / std::string are
replaced with fixed buffers for the embedded toolchain.

Safety: moveCartesian, movePulse, startJob, servoOn, and writeOutput can cause
manipulator motion or energization. Use risk assessment and Yaskawa safety docs.
*/

#ifndef MOTO_HSES_NB_H
#define MOTO_HSES_NB_H

#include <cstddef>
#include <cstdint>

#include <nettypes.h>

namespace moto {
namespace hses {

static const size_t kHeaderSize = 32;
static const size_t kMaxPayloadSize = 0x1DF;
static const size_t kMaxPacketSize = kHeaderSize + kMaxPayloadSize;
static const uint16_t kDefaultControlPort = 10040;
static const uint16_t kDefaultFilePort = 10041;
static const uint16_t kDefaultLocalUdpPort = 11040;
static const size_t kErrorCap = 96;

enum class Command : uint16_t {
    FileControl = 0x0000,
    AlarmRead = 0x0070,
    AlarmHistoryRead = 0x0071,
    StatusRead = 0x0072,
    ExecutingJobRead = 0x0073,
    AxisConfigRead = 0x0074,
    PositionRead = 0x0075,
    PositionErrorRead = 0x0076,
    TorqueRead = 0x0077,
    IoReadWrite = 0x0078,
    RegisterReadWrite = 0x0079,
    ByteVarReadWrite = 0x007A,
    IntVarReadWrite = 0x007B,
    DintVarReadWrite = 0x007C,
    RealVarReadWrite = 0x007D,
    Char16VarReadWrite = 0x007E,
    PositionVarReadWrite = 0x007F,
    BasePositionVarReadWrite = 0x0080,
    ExternalAxisVarReadWrite = 0x0081,
    AlarmResetErrorCancel = 0x0082,
    HoldServo = 0x0083,
    StepCycleContinuous = 0x0084,
    PendantStringDisplay = 0x0085,
    JobStart = 0x0086,
    JobSelect = 0x0087,
    ManagementTimeRead = 0x0088,
    SystemInfoRead = 0x0089,
    MoveCartesian = 0x008A,
    MovePulse = 0x008B,
    Char32VarReadWriteLegacy = 0x008C,
    Char32VarReadWrite = 0x008E,
    MultiIoReadWrite = 0x0300,
    MultiRegisterReadWrite = 0x0301,
    MultiByteVarReadWrite = 0x0302,
    MultiIntVarReadWrite = 0x0303,
    MultiDintVarReadWrite = 0x0304,
    MultiRealVarReadWrite = 0x0305,
    MultiChar16VarReadWrite = 0x0306,
    MultiPositionVarReadWrite = 0x0307,
    MultiBasePositionVarReadWrite = 0x0308,
    MultiExternalAxisVarReadWrite = 0x0309,
    AlarmReadWithSubCodeString = 0x030A,
    AlarmHistoryReadWithSubCodeString = 0x030B,
    MultiChar32VarReadWrite = 0x030C,
    EncoderTemperatureRead = 0x0411,
    ConverterTemperatureRead = 0x0413
};

enum class Service : uint8_t {
    GetAttributeAll = 0x01,
    SetAttributeAll = 0x02,
    GetAttributeSingle = 0x0E,
    SetAttributeSingle = 0x10,
    FileDelete = 0x09,
    FileLoad = 0x15,
    FileSave = 0x16,
    FileList = 0x32
};

enum class ControllerProfile : uint8_t {
    DX200 = 0,
    YRC1000 = 1,
    YRC1000micro = 2
};

enum class MoveCartesianOperation : uint16_t {
    LinkAbsolute = 1,
    StraightAbsolute = 2,
    StraightIncrement = 3
};

enum class MovePulseOperation : uint16_t {
    LinkAbsolute = 1,
    StraightAbsolute = 2
};

enum class MoveSpeedClass : uint32_t {
    LinkPercent = 0,
    V = 1,
    VR = 2
};

enum class MoveCoordinate : uint32_t {
    Base = 16,
    Robot = 17,
    User = 18,
    Tool = 19
};

struct Endpoint {
    IPADDR4 ip{};
    uint16_t port = kDefaultControlPort;
    // 0 = auto: 10041 when port==10040, else same as port
    uint16_t file_port = 0;
};

struct PositionVariableData {
    uint32_t data_type = 17;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    int32_t rx = 0;
    int32_t ry = 0;
    int32_t rz = 0;
};

struct PositionReadData {
    int32_t data_type = 0;
    int32_t form = 0;
    int32_t tool_number = 0;
    int32_t user_coordinate_number = 0;
    int32_t extended_form = 0;
    int32_t axis_data[8]{};
    bool valid = false;
};

struct MoveCartesianData {
    uint32_t control_group_robot = 1;
    uint32_t control_group_station = 0;
    MoveSpeedClass speed_class = MoveSpeedClass::V;
    int32_t speed = 0;
    MoveCoordinate coordinate = MoveCoordinate::Base;
    int32_t x_um = 0;
    int32_t y_um = 0;
    int32_t z_um = 0;
    int32_t tx_0p0001deg = 0;
    int32_t ty_0p0001deg = 0;
    int32_t tz_0p0001deg = 0;
    uint32_t reservation_1 = 0;
    uint32_t reservation_2 = 0;
    uint32_t type = 0;
    uint32_t expanded_type = 0;
    uint32_t tool_no = 0;
    uint32_t user_coordinate_no = 0;
    int32_t base_axis_um[3]{};
    int32_t station_axis_pulse[6]{};

    void setXYZ_mm(double x_mm, double y_mm, double z_mm);
    void setTxTyTz_deg(double tx_deg, double ty_deg, double tz_deg);
    // Writes LE payload; returns byte count (104) or 0 on buffer too small.
    size_t serialize(uint8_t *out, size_t outCap) const;
};

struct MovePulseData {
    uint32_t control_group_robot = 1;
    uint32_t control_group_station = 0;
    MoveSpeedClass speed_class = MoveSpeedClass::LinkPercent;
    int32_t speed = 0;
    int32_t robot_axis_pulse[8]{};
    uint32_t tool_no = 0;
    int32_t base_axis_pulse[3]{};
    int32_t station_axis_pulse[6]{};

    size_t serialize(uint8_t *out, size_t outCap) const;
};

namespace decode {
bool positionRead(const uint8_t *payload, size_t payloadLen, PositionReadData &out);
} // namespace decode

struct StatusFlags {
    bool step_mode = false;
    bool one_cycle_mode = false;
    bool auto_continuous_mode = false;
    bool running = false;
    bool in_guard_safe = false;
    bool teach_mode = false;
    bool play_mode = false;
    bool command_remote = false;
    bool hold_pendant = false;
    bool hold_external = false;
    bool hold_command = false;
    bool alarm = false;
    bool error = false;
    bool servo_on = false;
    uint32_t data1 = 0;
    uint32_t data2 = 0;
};

struct Response {
    bool transport_ok = false;
    uint32_t block_no = 0;
    uint8_t request_id = 0;
    uint8_t service = 0;
    uint8_t status = 0;
    uint8_t added_status_size = 0;
    uint16_t added_status = 0;
    uint16_t command_no = 0;
    uint16_t instance = 0;
    uint8_t attribute = 0;
    uint8_t payload[kMaxPayloadSize]{};
    size_t payload_len = 0;
    char error[kErrorCap]{};

    bool ok() const { return transport_ok && status == 0; }
};

struct RequestOptions {
    uint8_t request_id = 0;
    int timeout_ms = 600;
    uint32_t block_no = 0;
    uint8_t ack = 0;
    uint8_t processing_division = 1;
};

class Client {
public:
    Client();
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool open(const Endpoint &endpoint,
              uint16_t local_port = kDefaultLocalUdpPort,
              char *error = nullptr,
              size_t errorCap = 0);
    void close();
    bool is_open() const;
    void setControllerProfile(ControllerProfile profile);
    ControllerProfile controllerProfile() const;

    Response request(Command command,
                     uint16_t instance,
                     uint8_t attribute,
                     Service service,
                     const uint8_t *payload = nullptr,
                     size_t payloadLen = 0,
                     const RequestOptions &options = RequestOptions());

    Response request(Command command,
                     uint16_t instance,
                     uint8_t attribute,
                     Service service,
                     const uint8_t *payload,
                     size_t payloadLen,
                     uint8_t request_id,
                     int timeout_ms);

    Response readCommand(Command command,
                         uint16_t instance,
                         uint8_t attribute,
                         bool read_all = true,
                         const RequestOptions &options = RequestOptions());
    Response writeCommand(Command command,
                          uint16_t instance,
                          uint8_t attribute,
                          const uint8_t *payload,
                          size_t payloadLen,
                          bool write_single = true,
                          const RequestOptions &options = RequestOptions());

    Response readStatusAll(uint8_t request_id = 0, int timeout_ms = 600);
    Response readAlarmData(uint16_t instance,
                           uint8_t attribute,
                           bool read_all = false,
                           uint8_t request_id = 0,
                           int timeout_ms = 600);
    Response readAlarmHistory(uint16_t instance,
                              uint8_t attribute,
                              bool read_all = false,
                              uint8_t request_id = 0,
                              int timeout_ms = 600);
    Response readExecutingJobInfo(uint16_t task_instance,
                                  uint8_t attribute,
                                  bool read_all = false,
                                  uint8_t request_id = 0,
                                  int timeout_ms = 600);
    Response readAxisConfiguration(uint16_t instance,
                                   uint8_t attribute,
                                   bool read_all = false,
                                   uint8_t request_id = 0,
                                   int timeout_ms = 600);
    Response readPositionCartesian(uint8_t request_id = 0,
                                   int timeout_ms = 600,
                                   uint16_t instance = 0x0065);
    Response readPositionPulse(uint8_t request_id = 0,
                               int timeout_ms = 600,
                               uint16_t instance = 0x0001);
    Response readPositionError(uint16_t instance,
                               uint8_t attribute,
                               bool read_all = false,
                               uint8_t request_id = 0,
                               int timeout_ms = 600);
    Response readTorqueData(uint16_t instance,
                            uint8_t attribute,
                            bool read_all = false,
                            uint8_t request_id = 0,
                            int timeout_ms = 600);
    Response readEncoderTemperature(uint16_t instance,
                                    uint8_t attribute = 1,
                                    bool read_all = true,
                                    uint8_t request_id = 0,
                                    int timeout_ms = 600);
    Response readConverterTemperature(uint16_t instance,
                                      uint8_t attribute = 1,
                                      bool read_all = true,
                                      uint8_t request_id = 0,
                                      int timeout_ms = 600);

    Response servoOn(uint8_t request_id = 0, int timeout_ms = 600);
    Response servoOff(uint8_t request_id = 0, int timeout_ms = 600);
    Response holdOn(uint8_t request_id = 0, int timeout_ms = 600);
    Response holdOff(uint8_t request_id = 0, int timeout_ms = 600);
    Response setHoldLock(bool enable, uint8_t request_id = 0, int timeout_ms = 600);
    Response setStepCycleContinuous(uint32_t mode, uint8_t request_id = 0, int timeout_ms = 600);
    Response displayPendantString(const char *text, uint8_t request_id = 0, int timeout_ms = 600);
    Response readManagementTime(uint8_t attribute,
                                bool read_all = false,
                                uint8_t request_id = 0,
                                int timeout_ms = 600,
                                uint16_t instance = 1);
    Response readSystemInformation(uint8_t attribute,
                                   bool read_all = false,
                                   uint8_t request_id = 0,
                                   int timeout_ms = 600,
                                   uint16_t instance = 1);
    Response alarmResetOrErrorCancel(uint32_t mode = 1, uint8_t request_id = 0, int timeout_ms = 600);

    Response readIo(uint16_t io_address, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeOutput(uint16_t io_address, uint8_t value, uint8_t request_id = 0, int timeout_ms = 600);
    Response readRegister(uint16_t reg, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeRegister(uint16_t reg, uint32_t value, uint8_t request_id = 0, int timeout_ms = 600);

    Response readByteVariable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeByteVariable(uint16_t index, uint8_t value, uint8_t request_id = 0, int timeout_ms = 600);
    Response readIntVariable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeIntVariable(uint16_t index, int16_t value, uint8_t request_id = 0, int timeout_ms = 600);
    Response readDintVariable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeDintVariable(uint16_t index, int32_t value, uint8_t request_id = 0, int timeout_ms = 600);
    Response readRealVariable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeRealVariable(uint16_t index, float value, uint8_t request_id = 0, int timeout_ms = 600);
    Response readChar16Variable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeChar16Variable(uint16_t index, const char *value, uint8_t request_id = 0, int timeout_ms = 600);
    Response readChar32Variable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeChar32Variable(uint16_t index, const char *value, uint8_t request_id = 0, int timeout_ms = 600);

    Response readPositionVariable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writePositionVariable(uint16_t index,
                                   const PositionVariableData &data,
                                   uint8_t request_id = 0,
                                   int timeout_ms = 600);
    Response readBasePositionVariable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeBasePositionVariable(uint16_t index,
                                       const PositionVariableData &data,
                                       uint8_t request_id = 0,
                                       int timeout_ms = 600);
    Response readExternalAxisVariable(uint16_t index, uint8_t request_id = 0, int timeout_ms = 600);
    Response writeExternalAxisVariable(uint16_t index,
                                       const PositionVariableData &data,
                                       uint8_t request_id = 0,
                                       int timeout_ms = 600);

    Response moveCartesianRaw(const uint8_t *payload, size_t payloadLen, uint8_t request_id = 0, int timeout_ms = 600);
    Response movePulseRaw(const uint8_t *payload, size_t payloadLen, uint8_t request_id = 0, int timeout_ms = 600);
    Response moveCartesian(const MoveCartesianData &data,
                           MoveCartesianOperation op = MoveCartesianOperation::StraightAbsolute,
                           uint8_t request_id = 0,
                           int timeout_ms = 600);
    Response movePulse(const MovePulseData &data,
                       MovePulseOperation op = MovePulseOperation::LinkAbsolute,
                       uint8_t request_id = 0,
                       int timeout_ms = 600);

    Response multiIoReadWrite(uint16_t instance,
                              uint8_t attribute,
                              const uint8_t *payload,
                              size_t payloadLen,
                              bool write_single,
                              uint8_t request_id = 0,
                              int timeout_ms = 600);
    Response multiRegisterReadWrite(uint16_t instance,
                                    uint8_t attribute,
                                    const uint8_t *payload,
                                    size_t payloadLen,
                                    bool write_single,
                                    uint8_t request_id = 0,
                                    int timeout_ms = 600);
    Response multiByteVarReadWrite(uint16_t instance,
                                   uint8_t attribute,
                                   const uint8_t *payload,
                                   size_t payloadLen,
                                   bool write_single,
                                   uint8_t request_id = 0,
                                   int timeout_ms = 600);
    Response multiIntVarReadWrite(uint16_t instance,
                                  uint8_t attribute,
                                  const uint8_t *payload,
                                  size_t payloadLen,
                                  bool write_single,
                                  uint8_t request_id = 0,
                                  int timeout_ms = 600);
    Response multiDintVarReadWrite(uint16_t instance,
                                   uint8_t attribute,
                                   const uint8_t *payload,
                                   size_t payloadLen,
                                   bool write_single,
                                   uint8_t request_id = 0,
                                   int timeout_ms = 600);
    Response multiRealVarReadWrite(uint16_t instance,
                                   uint8_t attribute,
                                   const uint8_t *payload,
                                   size_t payloadLen,
                                   bool write_single,
                                   uint8_t request_id = 0,
                                   int timeout_ms = 600);
    Response multiChar16VarReadWrite(uint16_t instance,
                                     uint8_t attribute,
                                     const uint8_t *payload,
                                     size_t payloadLen,
                                     bool write_single,
                                     uint8_t request_id = 0,
                                     int timeout_ms = 600);
    Response multiPositionVarReadWrite(uint16_t instance,
                                       uint8_t attribute,
                                       const uint8_t *payload,
                                       size_t payloadLen,
                                       bool write_single,
                                       uint8_t request_id = 0,
                                       int timeout_ms = 600);
    Response multiBasePositionVarReadWrite(uint16_t instance,
                                           uint8_t attribute,
                                           const uint8_t *payload,
                                           size_t payloadLen,
                                           bool write_single,
                                           uint8_t request_id = 0,
                                           int timeout_ms = 600);
    Response multiExternalAxisVarReadWrite(uint16_t instance,
                                           uint8_t attribute,
                                           const uint8_t *payload,
                                           size_t payloadLen,
                                           bool write_single,
                                           uint8_t request_id = 0,
                                           int timeout_ms = 600);
    Response multiChar32VarReadWrite(uint16_t instance,
                                     uint8_t attribute,
                                     const uint8_t *payload,
                                     size_t payloadLen,
                                     bool write_single,
                                     uint8_t request_id = 0,
                                     int timeout_ms = 600);

    Response selectJob(const char *job_name, uint32_t line_number = 0, uint8_t request_id = 0, int timeout_ms = 600);
    Response startJob(uint8_t request_id = 0, int timeout_ms = 600);

    // File control (UDP 10041 / processing_division=2) is not ported in this NetBurner build.

    static StatusFlags decodeStatus(const Response &response);
    static void statusSummary(const StatusFlags &status, char *out, size_t outCap);
    static const char *explainStatus(uint8_t status);
    static const char *explainAddedStatus(uint16_t added_status);

private:
    Command activeChar32Command() const;
    bool isUndefinedCommandResponse(const Response &response) const;
    Response char32RequestWithFallback(uint16_t index,
                                       Service service,
                                       const uint8_t *payload,
                                       size_t payloadLen,
                                       uint8_t request_id,
                                       int timeout_ms);

    struct Impl;
    Impl *impl_;
};

} // namespace hses
} // namespace moto

#endif // MOTO_HSES_NB_H
