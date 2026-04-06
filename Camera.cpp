// Camera.cpp
#include "Camera.h"
#include "InputDevice.h"
#include <cmath>

Camera::Camera()
    : position_(0.0f, 5.0f, -15.0f)
    , forward_(0.0f, 0.0f, 1.0f)
    , right_(1.0f, 0.0f, 0.0f)
    , up_(0.0f, 1.0f, 0.0f)
    , yaw_(-90.0f)
    , pitch_(0.0f)
    , speed_(15.0f)
    , rotationSpeed_(50.0f)  // Градусов в секунду
{
}

void Camera::Update(float deltaTime, const InputDevice& input)
{
    // Поворот камеры клавишами
    float rotationDelta = rotationSpeed_ * deltaTime;

    // Вращение влево/вправо (Yaw)
    if (input.IsKeyDown(VK_LEFT))
    {
        yaw_ -= rotationDelta;
    }
    if (input.IsKeyDown(VK_RIGHT))
    {
        yaw_ += rotationDelta;
    }

    // Вращение вверх/вниз (Pitch)
    if (input.IsKeyDown(VK_UP))
    {
        pitch_ -= rotationDelta;
    }
    if (input.IsKeyDown(VK_DOWN))
    {
        pitch_ += rotationDelta;
    }

    // Ограничиваем pitch, чтобы не перевернуться
    if (pitch_ > 89.0f)
        pitch_ = 89.0f;
    if (pitch_ < -89.0f)
        pitch_ = -89.0f;

    // Обновляем векторы камеры
    UpdateVectors();

    // Движение камеры (WASD)
    float moveSpeed = speed_ * deltaTime;
    XMFLOAT3 moveDelta = { 0.0f, 0.0f, 0.0f };

    // Движение вперед/назад
    if (input.IsKeyDown('W'))
    {
        moveDelta.x += forward_.x * moveSpeed;
        moveDelta.y += forward_.y * moveSpeed;
        moveDelta.z += forward_.z * moveSpeed;
    }
    if (input.IsKeyDown('S'))
    {
        moveDelta.x -= forward_.x * moveSpeed;
        moveDelta.y -= forward_.y * moveSpeed;
        moveDelta.z -= forward_.z * moveSpeed;
    }

    // Движение влево/вправо (страфинг)
    if (input.IsKeyDown('A'))
    {
        moveDelta.x -= right_.x * moveSpeed;
        moveDelta.y -= right_.y * moveSpeed;
        moveDelta.z -= right_.z * moveSpeed;
    }
    if (input.IsKeyDown('D'))
    {
        moveDelta.x += right_.x * moveSpeed;
        moveDelta.y += right_.y * moveSpeed;
        moveDelta.z += right_.z * moveSpeed;
    }

    // Движение вверх/вниз
    if (input.IsKeyDown('Q') || input.IsKeyDown(VK_PRIOR))
    {
        moveDelta.y += moveSpeed;
    }
    if (input.IsKeyDown('E') || input.IsKeyDown(VK_NEXT))
    {
        moveDelta.y -= moveSpeed;
    }

    position_.x += moveDelta.x;
    position_.y += moveDelta.y;
    position_.z += moveDelta.z;
}

void Camera::LookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up)
{
    position_ = eye;

    XMVECTOR eyeVec = XMLoadFloat3(&eye);
    XMVECTOR targetVec = XMLoadFloat3(&target);
    XMVECTOR upVec = XMLoadFloat3(&up);

    XMVECTOR forwardVec = XMVector3Normalize(targetVec - eyeVec);
    XMVECTOR rightVec = XMVector3Normalize(XMVector3Cross(upVec, forwardVec));
    XMVECTOR upVecCalc = XMVector3Cross(forwardVec, rightVec);

    XMStoreFloat3(&forward_, forwardVec);
    XMStoreFloat3(&right_, rightVec);
    XMStoreFloat3(&up_, upVecCalc);

    // Вычисляем yaw и pitch из forward
    yaw_ = atan2f(forward_.z, forward_.x) * 180.0f / XM_PI;
    pitch_ = asinf(forward_.y) * 180.0f / XM_PI;
}

XMMATRIX Camera::GetViewMatrix() const
{
    XMVECTOR pos = XMLoadFloat3(&position_);
    XMVECTOR target = pos + XMLoadFloat3(&forward_);
    XMVECTOR up = XMLoadFloat3(&up_);

    return XMMatrixLookAtLH(pos, target, up);
}

XMMATRIX Camera::GetProjectionMatrix(float aspectRatio) const
{
    return XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspectRatio, 0.1f, 1000.0f);
}

void Camera::UpdateVectors()
{
    // Конвертируем углы в радианы
    float yawRad = yaw_ * XM_PI / 180.0f;
    float pitchRad = pitch_ * XM_PI / 180.0f;

    // Вычисляем forward вектор
    forward_.x = cosf(yawRad) * cosf(pitchRad);
    forward_.y = sinf(pitchRad);
    forward_.z = sinf(yawRad) * cosf(pitchRad);

    // Нормализуем forward
    float len = sqrtf(forward_.x * forward_.x + forward_.y * forward_.y + forward_.z * forward_.z);
    if (len > 0.0f)
    {
        forward_.x /= len;
        forward_.y /= len;
        forward_.z /= len;
    }

    // Вычисляем right и up векторы
    XMVECTOR forwardVec = XMLoadFloat3(&forward_);
    XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMVECTOR rightVec = XMVector3Normalize(XMVector3Cross(worldUp, forwardVec));
    XMVECTOR upVec = XMVector3Cross(forwardVec, rightVec);

    XMStoreFloat3(&right_, rightVec);
    XMStoreFloat3(&up_, upVec);
}