#include "InputDevice.h"

InputDevice::InputDevice(HWND hwnd) : hwnd_(hwnd)
{
    keys_.fill(false);
    mouseBtn_.fill(false);
}

void InputDevice::Initialize()
{
    // Простая инициализация, без захвата мыши
}

void InputDevice::OnKeyDown(int key)
{
    if (key >= 0 && key < 256)
        keys_[key] = true;

    // ESC выходит из приложения
    if (key == VK_ESCAPE)
        PostQuitMessage(0);
}

void InputDevice::OnKeyUp(int key)
{
    if (key >= 0 && key < 256)
        keys_[key] = false;
}

void InputDevice::OnMouseMove(int x, int y)
{
    mouseX_ = x;
    mouseY_ = y;
    mouseDX_ = x - prevX_;
    mouseDY_ = y - prevY_;
    prevX_ = x;
    prevY_ = y;
}

void InputDevice::OnMouseButton(int button, bool pressed)
{
    if (button >= 0 && button < 3)
        mouseBtn_[button] = pressed;
}

bool InputDevice::IsKeyDown(int key) const
{
    if (key >= 0 && key < 256)
        return keys_[key];
    return false;
}

bool InputDevice::IsMouseDown(int btn) const
{
    if (btn >= 0 && btn < 3)
        return mouseBtn_[btn];
    return false;
}

void InputDevice::Update()
{
    // Сбрасываем дельту мыши
    mouseDX_ = 0;
    mouseDY_ = 0;
}