#include "GamepadController.h"

using LogGamepad = Tiny::TILogTarget<TinyCon::GamepadLogLevel>;
using LogI2C = Tiny::TILogTarget<TinyCon::I2CLogLevel>;

void TinyCon::GamepadController::Init(int8_t hatOffset, const std::array<int8_t, MaxNativeAdcPinCount>& axisPins, const std::array<int8_t, MaxNativeGpioPinCount>& buttonPins, ActiveState activeState)
{
    // Input -1 is always the device itself with raw ADC and GPIO pins
    Inputs[Inputs.size() - 1].Init(axisPins, buttonPins, activeState);
    for (std::size_t i = 0; i < Inputs.size() - 1; ++i) Inputs[i].Init(I2C0, i);
    auto anyMpuInitialized = false;
    for (std::size_t i = 0; i < Mpus.size(); ++i)
    {
        Mpus[i].Init(I2C0, i);
        if (!anyMpuInitialized) Mpus[i].Update();
        anyMpuInitialized |= Mpus[i].Present;
    }
    Haptics[1].Init(I2C0);
    Haptics[0].Init(I2C1);
    HatOffset = hatOffset;
}

void TinyCon::GamepadController::Update(uint32_t deltaTime)
{
    if constexpr (Tiny::GlobalLogThreshold >= Tiny::TILogLevel::Verbose)
    {
        LogI2C::Verbose("I2C0 Devices: ");
        bool found = false;
        for (auto addr = 0x02; addr < 0x78; ++addr)
        {
            I2C0.beginTransmission(addr);
            if (I2C0.endTransmission() == 0)
            {
                if (found) LogI2C::Verbose(", ");
                found = true;
                LogI2C::Verbose("0x", addr, Tiny::TIFormat::Hex);
            }
        }

        LogI2C::Verbose(Tiny::TIEndl);
        LogI2C::Verbose("I2C1 Devices: ");
        found = false;
        for (auto addr = 0x02; addr < 0x78; ++addr)
        {
            I2C1.beginTransmission(addr);
            if (I2C1.endTransmission() == 0)
            {
                if (found) LogI2C::Verbose(", ");
                found = true;
                LogI2C::Verbose("0x", addr, Tiny::TIFormat::Hex);
            }
        }
        LogI2C::Verbose(Tiny::TIEndl);
    }

    I2C0.setClock(400000);
    LogGamepad::Info("Controller Update:", Tiny::TIEndl);
    auto mpuInitialized = false;
    for (auto& mpu : Mpus)
    {
        auto time = millis();
        auto mpuWasPresent = mpu.Present;
        if (!mpuInitialized || mpu.Present) mpu.Update();
        if (!mpuWasPresent && mpu.Present) mpuInitialized = true;
        LogGamepad::Debug("    MPU: (", mpu.Acceleration.X, ", ", mpu.Acceleration.Y, ", ", mpu.Acceleration.Z,
                          "), (", mpu.AngularVelocity.X, ", ", mpu.AngularVelocity.Y, ", ", mpu.AngularVelocity.Z,
                          "), (", mpu.Orientation.X, ", ", mpu.Orientation.Y, ", ", mpu.Orientation.Z,
                          "), ", mpu.Temperature, ", ", millis() - time, "ms", Tiny::TIEndl);
    }

    I2C0.setClock(800000);
    for (auto& input : Inputs)
        if (input.Present)
        {
            auto time = millis();
            LogGamepad::Debug("    Input: (");
            input.Update();
            for (int8_t j = 0; j < input.GetAxisCount(); ++j)
            {
                if (j > 0) LogGamepad::Debug(", ");
                LogGamepad::Debug(input.Axis[j]);
            }
            LogGamepad::Debug("), (");
            for (int8_t j = 0; j < input.GetButtonCount(); ++j)
            {
                if (j > 0) LogGamepad::Debug(", ");
                LogGamepad::Debug(input.Buttons[j] ? "Down" : "Up");
            }
            LogGamepad::Debug("), ", millis() - time, "ms", Tiny::TIEndl);
        }

    I2C0.setClock(400000);
    for (auto& haptic : Haptics)
        if (haptic.Present && haptic.Enabled)
        {
            auto time = millis();
            LogGamepad::Debug("    Haptic: ", haptic.Available(), " ");
            haptic.Update(deltaTime);
            LogGamepad::Debug(", ", millis() - time, "ms");
            LogGamepad::Info(Tiny::TIEndl);
        }
}

#if !NO_BLE || !NO_USB
hid_gamepad_report_t TinyCon::GamepadController::MakeHidReport() const
{
    hid_gamepad_report_t report = {};

    // XXX: Figure out how we can support more than 2+1 axis per controller
    // Assume any extra axis is 0-ed if not available or the controller is disabled
    report.x = GetAxis(0) * 127;
    report.y = GetAxis(1) * 127;
    report.z = GetAxis(2) * 127;
    report.rz = GetAxis(3) * 127;
    report.rx = GetAxis(4) * 127;
    report.ry = GetAxis(5) * 127;

    if (HatOffset < 0) report.hat = GAMEPAD_HAT_CENTERED;
    else
    {
        // Figure out the hat, clock-wise starting at the top position button. Offsetting this by 5
        if (GetButton(HatOffset) && GetButton(HatOffset + 3)) report.hat = GAMEPAD_HAT_UP_LEFT;
        else if (GetButton(HatOffset) && GetButton(HatOffset + 1)) report.hat = GAMEPAD_HAT_UP_RIGHT;
        else if (GetButton(HatOffset)) report.hat = GAMEPAD_HAT_UP;
        else if (GetButton(HatOffset + 1) && GetButton(HatOffset + 2)) report.hat = GAMEPAD_HAT_DOWN_RIGHT;
        else if (GetButton(HatOffset + 1)) report.hat = GAMEPAD_HAT_RIGHT;
        else if (GetButton(HatOffset + 2) && GetButton(HatOffset + 3)) report.hat = GAMEPAD_HAT_DOWN_LEFT;
        else if (GetButton(HatOffset + 2)) report.hat = GAMEPAD_HAT_DOWN;
        else if (GetButton(HatOffset + 3)) report.hat = GAMEPAD_HAT_LEFT;
        else report.hat = GAMEPAD_HAT_CENTERED;
    }

    for (int8_t index = 0; index < 32; ++index)
        if (HatOffset < 0 || index < HatOffset) report.buttons |= GetButton(index) << (index);
        else report.buttons |= GetButton(index + 4) << (index);
    return report;
}
#endif

std::size_t TinyCon::GamepadController::MakeMpuBuffer(Tiny::Collections::TIFixedSpan<uint8_t> data) const
{
    auto size = 0;
    for (auto& mpu : Mpus) if (mpu.Present)
        size += mpu.FillBuffer({data.data() + size, data.size()});
    return size;
}

void TinyCon::GamepadController::AddHapticCommand(Tiny::Collections::TIFixedSpan<uint8_t> data)
{
    if (data.size() > 12)
    {
        for (int8_t bit = 0; bit < 8; ++bit)
            if ((data[0] & (1 << bit)) != 0)
            {
                uint8_t controller = bit;
                uint8_t command = data[1];
                uint8_t count = data[2];
                const uint8_t* sequence = data.data() + 3;
                uint16_t timeout = (data[11] << 8) | data[12];
                Haptics[controller].Insert(command, count, sequence, timeout);
                LogGamepad::Info("Add Haptic Command: ", controller, ", ", command, ", ", count, ", ", timeout, Tiny::TIEndl);
            }
    }
}

float TinyCon::GamepadController::GetAxis(int8_t axisIndex) const
{
    for (auto& input : Inputs)
        if (axisIndex < input.GetAxisCount()) return input.Axis[axisIndex];
        else axisIndex -= input.GetAxisCount();
    return 0.0f;
}

bool TinyCon::GamepadController::GetButton(int8_t buttonIndex) const
{
    for (auto& input : Inputs)
        if (buttonIndex < input.GetButtonCount()) return input.Buttons[buttonIndex];
        else buttonIndex -= input.GetButtonCount();
    return false;
}

bool TinyCon::GamepadController::GetUpdatedButton(int8_t buttonIndex) const
{
    for (auto& input : Inputs)
        if (buttonIndex < input.GetButtonCount()) return input.GetUpdatedButton(buttonIndex);
        else buttonIndex -= input.GetButtonCount();
    return false;
}

void TinyCon::GamepadController::Reset()
{
    Id = 0;
    for (auto& haptic : Haptics) haptic.Reset();
    for (auto& mpu : Mpus) mpu.Reset();
    for (auto& input : Inputs) input.Reset();
}
