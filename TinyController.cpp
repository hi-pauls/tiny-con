#include "TinyController.h"

using LogState = Tiny::TILogTarget<TinyCon::StateLogLevel>;

void TinyCon::TinyController::Init(int8_t hatOffset, const std::array<int8_t, MaxNativeAdcPinCount>& axisPins, const std::array<int8_t, MaxNativeGpioPinCount>& buttonPins, ActiveState activeState)
{
    Controller.Init(hatOffset, axisPins, buttonPins, activeState);
    Power.Init();
    Processor.Init();

#if !NO_I2C_SLAVE
    I2C.Init();
#endif

#if !NO_USB
    USBControl.Init();
    USBControl.SetActive(Power.PowerSource == PowerSources::USB);
#endif

#if !NO_BLE
    Bluetooth.Init();
    Bluetooth.SetActive(Power.PowerSource == PowerSources::Battery || !USBControl.IsActive());
#endif
    Indicators.Init();
}

void TinyCon::TinyController::Update(int32_t deltaTime)
{
    bool i2cNeedsUpdate = Power.PowerSource == PowerSources::I2C || (!Processor.GetUSBEnabled() && !Processor.GetBLEEnabled());
    bool bluetoothWasConnected = Bluetooth.IsConnected();
    bool bluetoothNeedsUpdate = !i2cNeedsUpdate && Bluetooth.IsActive();
    bool usbNeedsUpdate = !i2cNeedsUpdate && !Bluetooth.IsConnected() && USBControl.IsActive();
    if (i2cNeedsUpdate || bluetoothNeedsUpdate || usbNeedsUpdate)
    {
        LogState::Info("State: Updating", Tiny::TIEndl);
        Controller.Update(deltaTime);
        Processor.Update();
        if (bluetoothNeedsUpdate) Bluetooth.Update(deltaTime);
        if (usbNeedsUpdate) USBControl.Update();
        UpdateSelectButton(deltaTime, Controller.GetButton(BluetoothStartButtonIndex));
        Suspended = false;
    }
    else
    {
        LogState::Info("State: Suspended", Tiny::TIEndl);
        UpdateSelectButton(deltaTime, Controller.GetUpdatedButton(BluetoothStartButtonIndex));
        Suspended = true;
    }

    bool powerWasUsb = Power.PowerSource == PowerSources::USB;
    bool powerWasI2c = Power.PowerSource == PowerSources::I2C;
    Power.Update();
    bool powerIsUsb = Power.PowerSource == PowerSources::USB;
    if (Power.PowerSource == PowerSources::I2C)
    {
        LogState::Debug("I2C connected, disable USB and Bluetooth (again)", Tiny::TIEndl);
        USBControl.SetActive(false);
        Bluetooth.SetActive(false);
    }
    else if (powerWasI2c)
    {
        LogState::Debug("I2C disconnected, ");
        if (powerIsUsb)
        {
            LogState::Debug("USB power, enabling USB, disabling Bluetooth", Tiny::TIEndl);
            USBControl.SetActive(true);
            Bluetooth.SetActive(false);
        }
        else
        {
            LogState::Debug("Battery power, enabling Bluetooth, disabling USB", Tiny::TIEndl);
            USBControl.SetActive(false);
            Bluetooth.SetActive(true);
        }
    }
    else if (powerIsUsb)
    {
        if (!powerWasUsb)
        {
            LogState::Debug("USB connected, disabling Bluetooth", Tiny::TIEndl);
            USBControl.SetActive(true);
            Bluetooth.SetActive(false);
        }
        else if (bluetoothWasConnected && !Bluetooth.IsConnected())
        {
            LogState::Debug("Forced Bluetooth on USB disconnect, enabling USB, stopping auto-advertising", Tiny::TIEndl);
            USBControl.SetActive(true);
            Bluetooth.SetActive(false);
        }
    }
    else if (powerWasUsb)
    {
        LogState::Debug("USB disconnected, enabling Bluetooth", Tiny::TIEndl);
        USBControl.SetActive(false);
        Bluetooth.SetActive(true);
    }

    UpdateIndicators(deltaTime);
}

void TinyCon::TinyController::UpdateSelectButton(int32_t deltaTime, bool selectButton)
{
    if (selectButton)
        if (BluetoothStartPressedTimeout > 0) BluetoothStartPressedTimeout -= deltaTime;
        else
        {
            LogState::Info("Bluetooth Button Triggered", Tiny::TIEndl);
            Bluetooth.SetActive(!Bluetooth.IsActive());
            if (!Bluetooth.IsActive() && Power.PowerSource == PowerSources::USB) USBControl.SetActive(true);
            BluetoothStartPressedTimeout = BluetoothStartButtonTime;
        }
    else BluetoothStartPressedTimeout = BluetoothStartButtonTime;
}

void TinyCon::TinyController::UpdateIndicators(int32_t deltaTime)
{
    if (Suspended) Indicators.Disable();
    else
    {
        if (Bluetooth.IsAdvertising()) Indicators.SetBlue(IndicatorController::LedEffects::Fade);
        else if (Bluetooth.IsConnected()) Indicators.SetBlue(IndicatorController::LedEffects::Pulse);
        else Indicators.SetBlue(IndicatorController::LedEffects::Off);

        if (Power.PowerSource == PowerSources::I2C) Indicators.SetRed(IndicatorController::LedEffects::Pulse);
        else if (Bluetooth.IsConnected() || Power.PowerSource != PowerSources::USB) Indicators.SetRed(IndicatorController::LedEffects::Off);
        else if (USBControl.IsConnected()) Indicators.SetRed(IndicatorController::LedEffects::On);
        else Indicators.SetRed(IndicatorController::LedEffects::Fade);

#if USE_NEOPIXEL
        if (Power.PowerSource == PowerSources::USB)
        {
            if (Power.Battery.Voltage < 4.1f)
            {
                Indicators.SetRgbRed(IndicatorController::LedEffects::Pulse);
                Indicators.SetRgbGreen(IndicatorController::LedEffects::Off);
            }
            else
            {
                Indicators.SetRgbRed(IndicatorController::LedEffects::Off);
                Indicators.SetRgbGreen(IndicatorController::LedEffects::On);
            }

            Indicators.SetRgbBlue(IndicatorController::LedEffects::Off);
        }
        else if (Power.PowerSource == PowerSources::Battery)
        {
            if (Power.Battery.Percentage < 0.2f) Indicators.SetRgbRed(IndicatorController::LedEffects::Pulse);
            else Indicators.SetRgbRed(IndicatorController::LedEffects::Off);
            Indicators.SetRgbGreen(IndicatorController::LedEffects::Off);
            Indicators.SetRgbBlue(IndicatorController::LedEffects::Fixed, 127 * Power.Battery.Percentage);
        }
        else
        {
            Indicators.SetRgbRed(IndicatorController::LedEffects::Off);
            Indicators.SetRgbGreen(IndicatorController::LedEffects::Off);
            Indicators.SetRgbBlue(IndicatorController::LedEffects::Off);
        }
#endif

        char mode = 'D';
        if (Power.PowerSource == PowerSources::I2C) mode = 'I';
        else if (Bluetooth.IsConnected()) mode = 'B';
        else if (Bluetooth.IsAdvertising())
            if (USBControl.IsConnected()) mode = 'F';
            else mode = 'A';
        else if (USBControl.IsConnected()) mode = 'U';
        Indicators.Update(deltaTime, mode);
    }
}
