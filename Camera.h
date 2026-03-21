// Camera.h
#pragma once
#include <DirectXMath.h>
#include <Windows.h>

using namespace DirectX;

class InputDevice;

class Camera
{
public:
    Camera();

    void Update(float deltaTime, const InputDevice& input);
    void LookAt(const XMFLOAT3& eye, const XMFLOAT3& target, const XMFLOAT3& up);

    XMMATRIX GetViewMatrix() const;
    XMMATRIX GetProjectionMatrix(float aspectRatio) const;

    XMFLOAT3 GetPosition() const { return position_; }
    XMFLOAT3 GetForward() const { return forward_; }
    XMFLOAT3 GetRight() const { return right_; }
    XMFLOAT3 GetUp() const { return up_; }

    void SetPosition(const XMFLOAT3& pos) { position_ = pos; }
    void SetSpeed(float speed) { speed_ = speed; }
    void SetRotationSpeed(float speed) { rotationSpeed_ = speed; }

private:
    void UpdateVectors();

    XMFLOAT3 position_;
    XMFLOAT3 forward_;
    XMFLOAT3 right_;
    XMFLOAT3 up_;

    float yaw_;
    float pitch_;

    float speed_;
    float rotationSpeed_;
};