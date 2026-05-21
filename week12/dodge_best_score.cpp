/*
================================================================================
 Dodge++ (닷지없는 닷지게임)
 12주차 : 차별화 — 대시 스킬 + 무적 아이템
--------------------------------------------------------------------------------
 [12주차 추가 구현]
 A (로직)
   1. 대시 스킬            → PlayerController::TryDash()
                              - SPACE 입력 / 쿨타임 3초
                              - 이동 방향으로 순간이동 (거리: 180px)
                              - 쿨타임 중 재발동 불가
   2. 무적 아이템 스폰      → GameManager::SpawnInvincibleItem()
                              - 15초마다 화면 랜덤 위치에 1개 스폰
                              - 플레이어와 충돌 시 획득
   3. 무적 상태 타이머      → GameManager::UpdateInvincible()
                              - 지속 시간 5초
   4. 무적 중 충돌 판정 무시 → GameManager::Update() 내 isInvincible 체크
 B (렌더링)
   5. 대시 쿨타임 UI 게이지 → CooldownBarRenderer (신규 Component)
                              - 화면 좌하단 / NDC 직접 계산
   6. 무적 아이템 스프라이트 → 노란 별 모양 Mesh (삼각형 팬)
   7. 무적 발동 시 플레이어 깜빡임 → PlayerController::Render()
                              - 무적 중 0.1초 주기로 알파 토글
--------------------------------------------------------------------------------
 [수업 구조 유지]
  - ShaderSet / Material(추상) / ColorMaterial  ← 10~11주차 강의 그대로
  - Mesh::Create()                              ← GPU 버퍼 자기 소유
  - MeshRenderer(Mesh*, Material*)              ← 소유권 분리 (강의 6)
  - b0: World 행렬 (MeshRenderer)
  - b1: tintColor  (ColorMaterial::Bind)
--------------------------------------------------------------------------------
 [조원]
 조장 - 김정일 (12211586) - A (로직 담당)
 조원 - 안시헌 (12211645) - B (렌더링 담당)
================================================================================
*/

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <vector>
#include <chrono>
#include <string>
#include <random>
#include <cmath>

#pragma comment(linker, "/entry:WinMainCRTStartup /subsystem:console")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// ============================================================
// 스펙 상수
// ============================================================
static const int   SCREEN_W = 1280;
static const int   SCREEN_H = 720;

// 플레이어
static const float PLAYER_SPEED = 500.0f;
static const float PLAYER_HALF_W = 12.0f;
static const float PLAYER_HALF_H = 12.0f;

// 대시
static const float DASH_DISTANCE = 180.0f;  // 대시 이동 거리 (픽셀)
static const float DASH_COOLDOWN = 3.0f;    // 쿨타임 (초)

// 장애물
static const float OBS_HALF_W = 9.0f;
static const float OBS_HALF_H = 9.0f;
static const float OBS_SPEED_MIN = 150.0f;
static const float OBS_SPEED_MAX = 380.0f;
static const float OBS_SPAWN_INTERVAL = 1.2f;
static const int   OBS_MAX = 500;

// 무적 아이템
static const float ITEM_HALF_W = 14.0f;
static const float ITEM_HALF_H = 14.0f;
static const float ITEM_SPAWN_INTERVAL = 15.0f; // 스폰 주기 (초)
static const float INVINCIBLE_DURATION = 5.0f;  // 무적 지속 (초)

// 쿨다운 게이지 (픽셀 좌표)
static const int   GAUGE_X = 20;
static const int   GAUGE_Y = SCREEN_H - 40;
static const int   GAUGE_W = 160;
static const int   GAUGE_H = 18;

// 난이도 단계
static const float DIFFICULTY_INTERVAL = 20.0f;  // 단계 상승 주기 (초)
static const int   DIFFICULTY_MAX = 6;       // 최대 단계

// ============================================================
// 전역 게임 상태
// ============================================================
enum class GameState { MAIN, PLAYING, GAMEOVER };
GameState gState = GameState::MAIN;
float     gScore = 0.0f;
bool      gIsInvincible = false;   // 무적 여부
float     gInvTimer = 0.0f;    // 남은 무적 시간
float     gDashCooldown = 0.0f;    // 남은 쿨타임
float gHighScore = 0.0f;

// ============================================================
// 기본 구조체
// ============================================================
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT4 col;   // 셰이더에서 tintColor 로 덮어씌워짐
};

struct ConstantBuffer   // b0 : World 행렬
{
    XMMATRIX matWorld;
};

struct ColorBuffer      // b1 : 머티리얼 색상
{
    XMFLOAT4 tintColor;
};

// ============================================================
// ShaderSet (강의 구조 유지)
// ============================================================
struct ShaderSet
{
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;

    void Release()
    {
        if (vs) { vs->Release();     vs = nullptr; }
        if (ps) { ps->Release();     ps = nullptr; }
        if (layout) { layout->Release(); layout = nullptr; }
    }
};

// ============================================================
// Mesh (강의 구조 유지 — GPU 버퍼 자기 소유)
// ============================================================
class Mesh
{
public:
    ID3D11Buffer* vBuffer = nullptr;
    UINT          vertexCount = 0;

    ~Mesh() { if (vBuffer) { vBuffer->Release(); vBuffer = nullptr; } }

    void Create(ID3D11Device* device, const std::vector<Vertex>& vertices)
    {
        vertexCount = (UINT)vertices.size();
        D3D11_BUFFER_DESC   bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = sizeof(Vertex) * vertexCount;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = vertices.data();
        device->CreateBuffer(&bd, &sd, &vBuffer);
    }
};

// ============================================================
// Material (추상 기저 클래스)
// ============================================================
class Material
{
public:
    ShaderSet shaders;
    Material(ShaderSet s) : shaders(s) {}
    virtual ~Material() {}
    virtual void Bind(ID3D11DeviceContext* context) = 0;
};

// ============================================================
// ColorMaterial (강의 구조 유지)
// b1 슬롯에 tintColor 전송 → 단색 렌더링
// ============================================================
class ColorMaterial : public Material
{
public:
    XMFLOAT4      color;
    ID3D11Buffer* pColorBuffer = nullptr;

    ColorMaterial(ShaderSet s, XMFLOAT4 col, ID3D11Device* device)
        : Material(s), color(col)
    {
        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(ColorBuffer);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device->CreateBuffer(&cbd, nullptr, &pColorBuffer);
    }

    ~ColorMaterial() override
    {
        if (pColorBuffer) { pColorBuffer->Release(); pColorBuffer = nullptr; }
    }

    void SetColor(XMFLOAT4 col) { color = col; }

    void Bind(ID3D11DeviceContext* context) override
    {
        context->IASetInputLayout(shaders.layout);
        context->VSSetShader(shaders.vs, nullptr, 0);
        context->PSSetShader(shaders.ps, nullptr, 0);

        ColorBuffer cb = { color };
        context->UpdateSubresource(pColorBuffer, 0, nullptr, &cb, 0, 0);
        context->PSSetConstantBuffers(1, 1, &pColorBuffer);
    }
};

// ============================================================
// DeltaTime
// ============================================================
class DeltaTime
{
    std::chrono::high_resolution_clock::time_point prevTime;
public:
    DeltaTime() { prevTime = std::chrono::high_resolution_clock::now(); }
    float GetDelta()
    {
        auto  curr = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(curr - prevTime).count();
        prevTime = curr;
        if (dt > 0.05f) dt = 0.05f;
        return dt;
    }
};

// ============================================================
// WindowContext
// ============================================================
class WindowContext
{
public:
    HWND hWnd = nullptr;
    int  Width = SCREEN_W;
    int  Height = SCREEN_H;

    ~WindowContext() { UnregisterClass(L"DX11Engine", GetModuleHandle(NULL)); }

    bool Initialize(HINSTANCE hInst, LRESULT(CALLBACK* wndProc)(HWND, UINT, WPARAM, LPARAM))
    {
        WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"DX11Engine";
        if (!RegisterClassEx(&wc)) return false;

        RECT rc = { 0, 0, Width, Height };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        hWnd = CreateWindow(L"DX11Engine", L"Dodge++ | Week 12",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hInst, NULL);
        if (!hWnd) return false;

        ShowWindow(hWnd, SW_SHOW);
        printf("[Engine] Window Created. (%dx%d)\n", Width, Height);
        return true;
    }
};

// ============================================================
// GraphicsContext
// ============================================================
class GraphicsContext
{
public:
    ID3D11Device* Device = nullptr;
    ID3D11DeviceContext* ImmediateContext = nullptr;
    IDXGISwapChain* SwapChain = nullptr;
    ID3D11RenderTargetView* RTV = nullptr;
    int                     VSync = 1;

    bool InitDX(HWND hWnd, int w, int h)
    {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 1;
        sd.BufferDesc.Width = w; sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1; sd.Windowed = TRUE;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE,
            NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &SwapChain, &Device, NULL, &ImmediateContext);
        if (FAILED(hr)) { printf("[Error] DX11 Init failed.\n"); return false; }
        CreateRTV();
        printf("[Engine] DirectX 11 Initialized.\n");
        return true;
    }

    void CreateRTV()
    {
        if (RTV) RTV->Release();
        ID3D11Texture2D* pBB = nullptr;
        SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBB);
        Device->CreateRenderTargetView(pBB, NULL, &RTV);
        pBB->Release();
    }

    ShaderSet CompileAndCreate(const std::string& src,
        D3D11_INPUT_ELEMENT_DESC* ied, UINT iedCount)
    {
        ShaderSet res;
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errBlob = nullptr;

        D3DCompile(src.c_str(), src.length(), nullptr, nullptr, nullptr,
            "VS", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }

        D3DCompile(src.c_str(), src.length(), nullptr, nullptr, nullptr,
            "PS", "ps_5_0", 0, 0, &psBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }

        if (!vsBlob || !psBlob) return res;

        Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &res.vs);
        Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &res.ps);
        if (ied && iedCount > 0)
            Device->CreateInputLayout(ied, iedCount,
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &res.layout);

        vsBlob->Release(); psBlob->Release();
        return res;
    }

    ~GraphicsContext()
    {
        if (RTV)              RTV->Release();
        if (SwapChain)        SwapChain->Release();
        if (ImmediateContext) ImmediateContext->Release();
        if (Device)           Device->Release();
    }
};

// ============================================================
// Component / GameObject
// ============================================================
class GameObject;

class Component
{
public:
    GameObject* pOwner = nullptr;
    bool        isStarted = false;
    virtual void Start(GraphicsContext* gfx) = 0;
    virtual void Input() = 0;
    virtual void Update(float dt) = 0;
    virtual void Render(GraphicsContext* gfx) = 0;
    virtual ~Component() {}
};

class GameObject
{
public:
    float posX = 0.0f;
    float posY = 0.0f;
    float halfW = 0.0f;
    float halfH = 0.0f;
    bool  active = true;
    std::vector<Component*> components;

    GameObject(float x, float y, float hw = 0, float hh = 0)
        : posX(x), posY(y), halfW(hw), halfH(hh) {
    }

    ~GameObject() { for (auto* c : components) delete c; }

    void AddComponent(Component* c) { c->pOwner = this; components.push_back(c); }

    void Input()
    {
        for (auto* c : components) if (c) c->Input();
    }

    void Update(float dt, GraphicsContext* gfx)
    {
        for (auto* c : components)
        {
            if (!c) continue;
            if (!c->isStarted) { c->Start(gfx); c->isStarted = true; }
            c->Update(dt);
        }
    }

    void Render(GraphicsContext* gfx)
    {
        for (auto* c : components) if (c) c->Render(gfx);
    }
};

// ============================================================
// AABB 충돌 판정
// ============================================================
bool CheckAABB(const GameObject& a, const GameObject& b)
{
    return (a.posX - a.halfW < b.posX + b.halfW &&
        a.posX + a.halfW > b.posX - b.halfW &&
        a.posY - a.halfH < b.posY + b.halfH &&
        a.posY + a.halfH > b.posY - b.halfH);
}

// ============================================================
// MeshRenderer (강의 구조 유지 — 소유권 없이 참조만)
// ============================================================
class MeshRenderer : public Component
{
    Mesh* pMeshData = nullptr;
    Material* pMaterial = nullptr;
    ID3D11Buffer* cBuffer = nullptr;

public:
    MeshRenderer(Mesh* mesh, Material* mat) : pMeshData(mesh), pMaterial(mat) {}

    ~MeshRenderer() override
    {
        if (cBuffer) { cBuffer->Release(); cBuffer = nullptr; }
    }

    void Start(GraphicsContext* gfx) override
    {
        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(ConstantBuffer);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        gfx->Device->CreateBuffer(&cbd, nullptr, &cBuffer);
    }

    void Render(GraphicsContext* gfx) override
    {
        if (!pMeshData || !pMaterial) return;

        pMaterial->Bind(gfx->ImmediateContext);

        float ndcX = (pOwner->posX / SCREEN_W) * 2.0f - 1.0f;
        float ndcY = 1.0f - (pOwner->posY / SCREEN_H) * 2.0f;

        XMMATRIX world = XMMatrixTranslation(ndcX, ndcY, 0.0f);
        ConstantBuffer cb;
        cb.matWorld = XMMatrixTranspose(world);
        gfx->ImmediateContext->UpdateSubresource(cBuffer, 0, nullptr, &cb, 0, 0);
        gfx->ImmediateContext->VSSetConstantBuffers(0, 1, &cBuffer);

        UINT stride = sizeof(Vertex), offset = 0;
        gfx->ImmediateContext->IASetVertexBuffers(0, 1, &pMeshData->vBuffer, &stride, &offset);
        gfx->ImmediateContext->Draw(pMeshData->vertexCount, 0);
    }

    void Input()         override {}
    void Update(float)   override {}
};

// ============================================================
// PlayerController (A 담당)
// 12주차 추가: 대시 스킬 (SPACE / 쿨타임 3초)
//             무적 중 깜빡임은 별도 BlinkRenderer 컴포넌트가 담당
// ============================================================
class PlayerController : public Component
{
    XMFLOAT2 moveDir = { 0, 0 };

    // 대시 중 마지막 이동 방향 저장 (정지 상태에서 대시 시 이전 방향 유지)
    XMFLOAT2 lastDir = { 1, 0 };

public:
    void Start(GraphicsContext*) override
    {
        printf("[Player] Started. 방향키: 이동 | SPACE: 대시 | ESC: 종료\n");
    }

    void Input() override
    {
        moveDir = { 0, 0 };
        if (gState != GameState::PLAYING) return;

        if (GetAsyncKeyState(VK_UP) & 0x8000) moveDir.y -= 1.0f;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) moveDir.y += 1.0f;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) moveDir.x -= 1.0f;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) moveDir.x += 1.0f;
    }

    void Update(float dt) override
    {
        if (gState != GameState::PLAYING) return;

        // 쿨타임 감소
        if (gDashCooldown > 0.0f)
            gDashCooldown -= dt;

        // 이동 방향 저장 (정규화)
        float len = sqrtf(moveDir.x * moveDir.x + moveDir.y * moveDir.y);
        if (len > 0.0f) { lastDir.x = moveDir.x / len; lastDir.y = moveDir.y / len; }

        // 일반 이동
        pOwner->posX += moveDir.x * PLAYER_SPEED * dt;
        pOwner->posY += moveDir.y * PLAYER_SPEED * dt;

        // 화면 경계
        if (pOwner->posX < PLAYER_HALF_W)            pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W) pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)            pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H) pOwner->posY = SCREEN_H - PLAYER_HALF_H;

        // ── 대시 발동 (SPACE, 쿨타임 완료 시) ──────────────
        if ((GetAsyncKeyState(VK_SPACE) & 0x0001) && gDashCooldown <= 0.0f)
            TryDash();
    }

    // 현재 이동 방향으로 DASH_DISTANCE 만큼 순간이동
    void TryDash()
    {
        pOwner->posX += lastDir.x * DASH_DISTANCE;
        pOwner->posY += lastDir.y * DASH_DISTANCE;

        // 경계 클램프
        if (pOwner->posX < PLAYER_HALF_W)            pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W) pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)            pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H) pOwner->posY = SCREEN_H - PLAYER_HALF_H;

        gDashCooldown = DASH_COOLDOWN;
        printf("[Dash] 발동! 쿨타임 %.1f초 시작\n", DASH_COOLDOWN);
    }

    void Render(GraphicsContext*) override {}
};

// ============================================================
// BlinkRenderer (B 담당 — 12주차 신규)
// 무적 중 0.1초 주기로 플레이어를 깜빡이게 함
// MeshRenderer 의 색상을 직접 조작하는 방식 대신
// ColorMaterial 의 알파값을 토글해서 구현
// ============================================================
class BlinkRenderer : public Component
{
    ColorMaterial* pMat = nullptr;   // 플레이어 머티리얼 참조 (소유권 없음)
    float          blinkTimer = 0.0f;
    bool           visible = true;

    static const float BLINK_INTERVAL; // 깜빡임 주기 (초)

public:
    BlinkRenderer(ColorMaterial* mat) : pMat(mat) {}

    void Start(GraphicsContext*) override {}
    void Input()                 override {}

    void Update(float dt) override
    {
        if (!pMat) return;

        if (gIsInvincible)
        {
            // 무적 중: 0.1초마다 visible 토글
            blinkTimer += dt;
            if (blinkTimer >= BLINK_INTERVAL)
            {
                visible = !visible;
                blinkTimer = 0.0f;
            }
            // 알파값으로 깜빡임 표현
            float alpha = visible ? 1.0f : 0.0f;
            pMat->SetColor({ 1.0f, 1.0f, 1.0f, alpha });
        }
        else
        {
            // 평상시: 항상 완전 불투명
            visible = true;
            blinkTimer = 0.0f;
            pMat->SetColor({ 0.3f, 0.8f, 1.0f, 1.0f });
        }
    }

    void Render(GraphicsContext*) override {}
};
const float BlinkRenderer::BLINK_INTERVAL = 0.1f;

// ============================================================
// ObstacleController (A 담당 - 11주차 이월)
// ============================================================
class ObstacleController : public Component
{
public:
    float velX = 0.0f;
    float velY = 0.0f;

    void Start(GraphicsContext*) override {}
    void Input()  override {}

    void Update(float dt) override
    {
        if (!pOwner->active) return;
        pOwner->posX += velX * dt;
        pOwner->posY += velY * dt;

        if (pOwner->posY > SCREEN_H + OBS_HALF_H * 2) pOwner->active = false;
        if (pOwner->posY < -OBS_HALF_H * 2)             pOwner->active = false;
        if (pOwner->posX > SCREEN_W + OBS_HALF_W * 2) pOwner->active = false;
        if (pOwner->posX < -OBS_HALF_W * 2)             pOwner->active = false;
    }

    void Render(GraphicsContext*) override {}
};

// ============================================================
// CooldownBarRenderer (B 담당 — 12주차 신규)
// 대시 쿨타임 게이지를 GDI로 화면 좌하단에 렌더링
// ============================================================
class CooldownBarRenderer : public Component
{
    HWND hWnd = nullptr;
public:
    CooldownBarRenderer(HWND hwnd) : hWnd(hwnd) {}

    void Start(GraphicsContext*) override {}
    void Input()  override {}
    void Update(float) override {}

    void Render(GraphicsContext*) override
    {
        if (gState != GameState::PLAYING) return;

        HDC hdc = GetDC(hWnd);

        // 배경 (어두운 회색 테두리)
        HBRUSH bgBrush = CreateSolidBrush(RGB(50, 50, 50));
        RECT bgRect = { GAUGE_X - 2, GAUGE_Y - 2,
                           GAUGE_X + GAUGE_W + 2, GAUGE_Y + GAUGE_H + 2 };
        FillRect(hdc, &bgRect, bgBrush);
        DeleteObject(bgBrush);

        // 쿨타임 채워진 비율 계산 (0.0 = 완충, 1.0 = 비어있음)
        float ratio = (gDashCooldown <= 0.0f) ? 1.0f
            : 1.0f - (gDashCooldown / DASH_COOLDOWN);
        int   fillW = (int)(GAUGE_W * ratio);

        // 게이지 색상: 완충이면 하늘색, 충전 중이면 어두운 청색
        COLORREF gaugeColor = (gDashCooldown <= 0.0f)
            ? RGB(100, 220, 255) : RGB(40, 100, 180);

        HBRUSH gaugeBrush = CreateSolidBrush(gaugeColor);
        RECT   gaugeRect = { GAUGE_X, GAUGE_Y, GAUGE_X + fillW, GAUGE_Y + GAUGE_H };
        FillRect(hdc, &gaugeRect, gaugeBrush);
        DeleteObject(gaugeBrush);

        // 텍스트 레이블
        HFONT font = CreateFont(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(220, 220, 220));

        std::wstring label = (gDashCooldown <= 0.0f)
            ? L"DASH  READY"
            : (L"DASH  " + std::to_wstring((int)(gDashCooldown + 1.0f)) + L"s");
        TextOut(hdc, GAUGE_X, GAUGE_Y - 20, label.c_str(), (int)label.length());

        DeleteObject(font);
        ReleaseDC(hWnd, hdc);
    }
};

// ============================================================
// TextRenderer (B 담당 - 11주차 이월 + 12주차 UI 추가)
// ============================================================
class TextRenderer
{
    HWND  hWnd = nullptr;
    HFONT hFont = nullptr;
    HFONT hFontLarge = nullptr;

    // * 추가 — 별 위치 저장
    struct Star { int x, y; int brightness; };
    std::vector<Star> stars;

public:
    void Initialize(HWND hwnd)
    {
        hWnd = hwnd;
        hFont = CreateFont(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        hFontLarge = CreateFont(56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");

        // ★ 추가 — 별 200개 랜덤 생성
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> randX(0, SCREEN_W);
        std::uniform_int_distribution<int> randY(0, SCREEN_H);
        std::uniform_int_distribution<int> randB(100, 255); // 밝기
        for (int i = 0; i < 200; ++i)
            stars.push_back({ randX(rng), randY(rng), randB(rng) });
    }

    void DrawText(const std::wstring& text, int x, int y,
        COLORREF color, bool large = false)
    {
        HDC hdc = GetDC(hWnd);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        SelectObject(hdc, large ? hFontLarge : hFont);
        TextOut(hdc, x, y, text.c_str(), (int)text.length());
        ReleaseDC(hWnd, hdc);
    }

    void Render()
    {
        // ★ 추가 — 별 배경 (항상 그림)
        HDC hdc = GetDC(hWnd);
        for (auto& s : stars)
        {
            COLORREF c = RGB(s.brightness, s.brightness, s.brightness);
            SetPixel(hdc, s.x, s.y, c);  // 1픽셀 점
        }
        ReleaseDC(hWnd, hdc);

        if (gState == GameState::MAIN)
        {
            DrawText(L"DODGE++", 430, 200, RGB(255, 255, 255), true);
            DrawText(L"닷지없는 닷지게임", 430, 280, RGB(180, 180, 255));
            DrawText(L"SPACE  :  게임 시작", 460, 370, RGB(200, 200, 200));
            DrawText(L"방향키 :  이동", 460, 410, RGB(200, 200, 200));
            DrawText(L"SPACE  :  대시 (쿨 3초)", 460, 450, RGB(100, 220, 255));
            DrawText(L"★ 노란 별 = 무적 아이템", 460, 490, RGB(255, 230, 50));
            DrawText(L"ESC    :  종료", 460, 530, RGB(200, 200, 200));
        }
        else if (gState == GameState::PLAYING)
        {
            // 점수
            std::wstring scoreText = L"SCORE : " + std::to_wstring((int)gScore);
            DrawText(scoreText, 10, 10, RGB(255, 255, 100));

            // 무적 상태 표시
            if (gIsInvincible)
            {
                std::wstring invText = L"★ 무적  " +
                    std::to_wstring((int)(gInvTimer + 1.0f)) + L"s";
                DrawText(invText, SCREEN_W / 2 - 80, 10, RGB(255, 220, 50));
            }
        }
        else if (gState == GameState::GAMEOVER)
        {
            DrawText(L"GAME OVER", 440, 240, RGB(255, 80, 80), true);
            std::wstring scoreText = L"SCORE : " + std::to_wstring((int)gScore);
            DrawText(scoreText, 510, 340, RGB(255, 255, 100));

            // ★ 추가
            std::wstring hiText = L"BEST  : " + std::to_wstring((int)gHighScore);
            DrawText(hiText, 510, 380, RGB(100, 255, 180));

            DrawText(L"R  :  재시작", 510, 430, RGB(200, 200, 200));
            DrawText(L"ESC  :  종료", 510, 470, RGB(200, 200, 200));
        }
    }

    ~TextRenderer()
    {
        if (hFont)      DeleteObject(hFont);
        if (hFontLarge) DeleteObject(hFontLarge);
    }
};

// ============================================================
// GameManager (A 담당)
// 12주차 추가: 무적 아이템 스폰 / 획득 판정 / 무적 타이머
// ============================================================
class GameManager
{
    std::mt19937                          rng;
    std::uniform_real_distribution<float> randPosX;
    std::uniform_real_distribution<float> randPosY;
    std::uniform_real_distribution<float> randSpeed;
    std::uniform_int_distribution<int>    randSide;
    std::uniform_real_distribution<float> randDiag;

    float spawnTimer = 0.0f;   // 장애물 스폰 타이머
    float itemSpawnTimer = 0.0f;   // 아이템 스폰 타이머

    int   difficultyLevel = 0;      // 현재 난이도 단계 (0~6)
    float difficultyTimer = 0.0f;   // 단계 상승 타이머

public:
    std::vector<GameObject*>* obstacles = nullptr;
    std::vector<GameObject*>* items = nullptr;   // 무적 아이템 풀
    GameObject* player = nullptr;
    GraphicsContext* gfx = nullptr;

    // 공유 리소스 (소유권 WinMain)
    Mesh* obsMesh = nullptr;
    Material* obsMat = nullptr;
    Mesh* itemMesh = nullptr;   // 별 모양 Mesh
    Material* itemMat = nullptr;   // 노란색 Material

    GameManager()
        : rng(std::random_device{}())
        , randPosX(ITEM_HALF_W + 50, SCREEN_W - ITEM_HALF_W - 50)
        , randPosY(ITEM_HALF_H + 50, SCREEN_H - ITEM_HALF_H - 50)
        , randSpeed(OBS_SPEED_MIN, OBS_SPEED_MAX)
        , randSide(0, 3)
        , randDiag(-1.0f, 1.0f)
    {
    }

    void CalcObsSpawn(float& outX, float& outY, float& outVX, float& outVY)
    {
        float sMin, sMax;
        GetSpeedRange(sMin, sMax);
        std::uniform_real_distribution<float> dynSpeed(sMin, sMax);   // 단계별 범위

        int   side = randSide(rng);
        float speed = dynSpeed(rng);
        float diag = randDiag(rng);
        switch (side)
        {
        case 0: outX = randPosX(rng); outY = -OBS_HALF_H;
            outVX = speed * diag; outVY = speed; break;
        case 1: outX = randPosX(rng); outY = SCREEN_H + OBS_HALF_H;
            outVX = speed * diag; outVY = -speed; break;
        case 2: outX = -OBS_HALF_W;  outY = randPosY(rng);
            outVX = speed; outVY = speed * diag; break;
        default:outX = SCREEN_W + OBS_HALF_W; outY = randPosY(rng);
            outVX = -speed; outVY = speed * diag; break;
        }
    }

    void ResetGame()
    {
        gScore = 0.0f;
        spawnTimer = 0.0f;
        itemSpawnTimer = 0.0f;
        gIsInvincible = false;
        gInvTimer = 0.0f;
        gDashCooldown = 0.0f;
        difficultyLevel = 0;
        difficultyTimer = 0.0f;

        if (player)
        {
            player->posX = SCREEN_W / 2.0f;
            player->posY = SCREEN_H / 2.0f;
            player->active = true;
        }
        if (obstacles) for (auto* o : *obstacles) o->active = false;
        if (items)     for (auto* i : *items)     i->active = false;

        printf("[Game] Reset. PLAYING 시작!\n");
    }

    // ── 장애물 스폰 (오브젝트 풀 방식) ───────────────────
    void SpawnObstacles(int count)
    {
        for (int n = 0; n < count; ++n)
        {
            // 풀에서 비활성 오브젝트 재사용
            bool reused = false;
            for (auto* obj : *obstacles)
            {
                if (!obj->active)
                {
                    float x, y, vx, vy;
                    CalcObsSpawn(x, y, vx, vy);
                    obj->posX = x; obj->posY = y; obj->active = true;
                    auto* ctrl = dynamic_cast<ObstacleController*>(obj->components[0]);
                    if (ctrl) { ctrl->velX = vx; ctrl->velY = vy; }
                    reused = true;
                    break;
                }
            }
            if (reused) continue;

            // 풀 한도 초과 시 신규 생성
            if ((int)obstacles->size() >= OBS_MAX) break;

            float x, y, vx, vy;
            CalcObsSpawn(x, y, vx, vy);
            auto* obs = new GameObject(x, y, OBS_HALF_W, OBS_HALF_H);
            auto* ctrl = new ObstacleController();
            ctrl->velX = vx; ctrl->velY = vy;
            obs->AddComponent(ctrl);
            obs->AddComponent(new MeshRenderer(obsMesh, obsMat));
            obstacles->push_back(obs);
        }
    }

    // ── 무적 아이템 스폰 (12주차 신규) ───────────────────
    void SpawnInvincibleItem()
    {
        if (!items || !gfx || !itemMesh || !itemMat) return;

        // 화면에 이미 활성 아이템이 있으면 스킵 (동시에 1개만)
        for (auto* it : *items)
            if (it->active) return;

        float x = randPosX(rng);
        float y = randPosY(rng);

        // 풀에서 재사용
        for (auto* it : *items)
        {
            if (!it->active)
            {
                it->posX = x; it->posY = y; it->active = true;
                printf("[Item] 무적 아이템 스폰 (%.0f, %.0f)\n", x, y);
                return;
            }
        }

        // 새로 생성
        auto* item = new GameObject(x, y, ITEM_HALF_W, ITEM_HALF_H);
        item->AddComponent(new MeshRenderer(itemMesh, itemMat));
        items->push_back(item);
        printf("[Item] 무적 아이템 스폰 (%.0f, %.0f)\n", x, y);
    }

    // ── 무적 상태 처리 (12주차 신규) ─────────────────────
    void UpdateInvincible(float dt)
    {
        if (!gIsInvincible) return;
        gInvTimer -= dt;
        if (gInvTimer <= 0.0f)
        {
            gIsInvincible = false;
            gInvTimer = 0.0f;
            printf("[Invincible] 무적 종료\n");
        }
    }

    // 현재 단계에 따른 스폰 간격 반환
    float GetSpawnInterval() const
    {
        // 단계 0: 1.2s  → 단계6: 0.4s  (선형 감소)
        float t = (float)difficultyLevel / DIFFICULTY_MAX;
        return OBS_SPAWN_INTERVAL * (1.0f - t * 0.67f);   // 최소 약 0.4s
    }

    // 현재 단계에 따른 속도 범위 반환
    void GetSpeedRange(float& outMin, float& outMax) const
    {
        // 단계마다 +40 / +60 씩 증가
        outMin = OBS_SPEED_MIN + difficultyLevel * 40.0f;
        outMax = OBS_SPEED_MAX + difficultyLevel * 60.0f;
    }

    // 현재 단계에서 한 번에 스폰할 개수
    int GetSpawnCount() const
    {
        return 4 + difficultyLevel;
    }

    void Update(float dt)
    {
        if (gState != GameState::PLAYING) return;

        gScore += dt;

        // ── 난이도 단계 상승 (20초마다) ──────────────────────
        difficultyTimer += dt;
        if (difficultyTimer >= DIFFICULTY_INTERVAL && difficultyLevel < DIFFICULTY_MAX)
        {
            difficultyLevel++;
            difficultyTimer = 0.0f;
            printf("[Difficulty] 단계 %d 돌입! 속도↑ 스폰 간격↓ 동시스폰 %d개\n",
                difficultyLevel, GetSpawnCount());
        }

        // 장애물 스폰 (단계별 간격 + 동시 개수)
        spawnTimer += dt;
        if (spawnTimer >= GetSpawnInterval())
        {
            SpawnObstacles(GetSpawnCount());
            spawnTimer = 0.0f;
        }

        // 아이템 스폰 (15초마다) — 기존 유지
        itemSpawnTimer += dt;
        if (itemSpawnTimer >= ITEM_SPAWN_INTERVAL)
        {
            SpawnInvincibleItem(); itemSpawnTimer = 0.0f;
        }

        UpdateInvincible(dt);

        // 아이템 획득 판정 — 기존 유지
        if (player && player->active && items)
        {
            for (auto* it : *items)
            {
                if (!it->active) continue;
                if (CheckAABB(*player, *it))
                {
                    it->active = false;
                    gIsInvincible = true;
                    gInvTimer = INVINCIBLE_DURATION;
                    printf("[Invincible] 무적 획득! %.1f초\n", INVINCIBLE_DURATION);
                }
            }
        }

        // 충돌 판정 — 기존 유지
        if (!gIsInvincible && player && player->active && obstacles)
        {
            for (auto* obs : *obstacles)
            {
                if (!obs->active) continue;
                if (CheckAABB(*player, *obs))
                {
                    gState = GameState::GAMEOVER;
                    if (gScore > gHighScore) gHighScore = gScore;
                    printf("[Game] GAME OVER! Score: %.1f\n", gScore);
                    break;
                }
            }
        }
    }
};

// ============================================================
// GameLoop
// ============================================================
class GameLoop
{
public:
    WindowContext            win;
    GraphicsContext          gfx;
    DeltaTime                timer;
    std::vector<GameObject*> world;
    std::vector<GameObject*> obstacles;
    std::vector<GameObject*> items;      // 무적 아이템 풀
    TextRenderer             text;
    GameManager              manager;
    bool                     isRunning = true;

    GameLoop() { printf("[Engine] GameLoop Created.\n"); }
    ~GameLoop()
    {
        for (auto* o : world)     delete o;
        for (auto* o : obstacles) delete o;
        for (auto* o : items)     delete o;
        printf("[Engine] GameLoop Destroyed.\n");
    }

    void Initialize(HINSTANCE hInst, LRESULT(CALLBACK* wndProc)(HWND, UINT, WPARAM, LPARAM))
    {
        win.Initialize(hInst, wndProc);
        gfx.InitDX(win.hWnd, SCREEN_W, SCREEN_H);
        text.Initialize(win.hWnd);
    }

    void Input()
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) isRunning = false;

        if (gState == GameState::MAIN && (GetAsyncKeyState(VK_SPACE) & 0x0001))
        {
            gState = GameState::PLAYING; manager.ResetGame();
        }

        if (gState == GameState::GAMEOVER && (GetAsyncKeyState('R') & 0x0001))
        {
            gState = GameState::PLAYING; manager.ResetGame();
        }

        for (auto* obj : world) if (obj && obj->active) obj->Input();
    }

    void Update()
    {
        float dt = timer.GetDelta();
        manager.Update(dt);
        for (auto* obj : world)     if (obj && obj->active) obj->Update(dt, &gfx);
        for (auto* obs : obstacles) if (obs && obs->active) obs->Update(dt, &gfx);
        for (auto* it : items)     if (it && it->active)  it->Update(dt, &gfx);
    }

    void Render()
    {
        float col[] = { 0.0f, 0.0f, 0.05f, 1.0f };
        gfx.ImmediateContext->ClearRenderTargetView(gfx.RTV, col);

        D3D11_VIEWPORT vp = { 0, 0, (float)SCREEN_W, (float)SCREEN_H, 0, 1 };
        gfx.ImmediateContext->RSSetViewports(1, &vp);
        gfx.ImmediateContext->OMSetRenderTargets(1, &gfx.RTV, NULL);
        gfx.ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        ID3D11BlendState* blendState = nullptr;
        gfx.Device->CreateBlendState(&bd, &blendState);
        float blendFactor[4] = { 0,0,0,0 };
        gfx.ImmediateContext->OMSetBlendState(blendState, blendFactor, 0xffffffff);
        blendState->Release();

        for (auto* obs : obstacles) if (obs && obs->active) obs->Render(&gfx);
        for (auto* it : items)     if (it && it->active)  it->Render(&gfx);
        for (auto* obj : world)     if (obj && obj->active) obj->Render(&gfx);

        gfx.SwapChain->Present(gfx.VSync, 0);
        text.Render(); // GDI 오버레이 (Present 이후)
    }

    void Run()
    {
        MSG msg = {};
        while (msg.message != WM_QUIT && isRunning)
        {
            if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg); DispatchMessage(&msg);
            }
            else { Input(); Update(); Render(); }
        }
    }
};

// ============================================================
// 셰이더 소스 (11주차와 동일)
// b0: World 행렬  b1: tintColor
// ============================================================
static const std::string SHADER_SRC = R"(
    cbuffer cbWorld    : register(b0) { matrix matWorld;   }
    cbuffer cbMaterial : register(b1) { float4 tintColor; }

    struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };
    struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };

    PS_IN VS(VS_IN input)
    {
        PS_IN output;
        output.pos = mul(float4(input.pos, 1.0f), matWorld);
        output.col = input.col;
        return output;
    }

    float4 PS(PS_IN input) : SV_Target { return tintColor; }
)";

// ============================================================
// 사각형 Mesh 생성 헬퍼 (NDC 단위)
// ============================================================
Mesh* CreateRectMesh(ID3D11Device* device, float halfW, float halfH)
{
    float nW = (halfW / SCREEN_W) * 2.0f;
    float nH = (halfH / SCREEN_H) * 2.0f;

    std::vector<Vertex> v = {
        { { -nW,  nH, 0 }, { 1,1,1,1 } }, { {  nW,  nH, 0 }, { 1,1,1,1 } },
        { {  nW, -nH, 0 }, { 1,1,1,1 } }, { { -nW,  nH, 0 }, { 1,1,1,1 } },
        { {  nW, -nH, 0 }, { 1,1,1,1 } }, { { -nW, -nH, 0 }, { 1,1,1,1 } },
    };
    Mesh* mesh = new Mesh();
    mesh->Create(device, v);
    return mesh;
}

// ============================================================
// 별 Mesh 생성 헬퍼 (무적 아이템용 — B 담당)
// 5각형 별: 바깥 꼭짓점 5개 + 안쪽 꼭짓점 5개 → 삼각형 10개
// ============================================================
Mesh* CreateStarMesh(ID3D11Device* device, float outerR, float innerR)
{
    float nOutX = (outerR / SCREEN_W) * 2.0f;
    float nOutY = (outerR / SCREEN_H) * 2.0f;
    float nInX = (innerR / SCREEN_W) * 2.0f;
    float nInY = (innerR / SCREEN_H) * 2.0f;

    // 10개의 꼭짓점 (바깥 / 안쪽 교대)
    XMFLOAT2 pts[10];
    for (int i = 0; i < 10; ++i)
    {
        // XM_PIDIV2 에서 시작 → 꼭짓점이 위쪽을 향함
        float angle = XM_PIDIV2 - (i * XM_2PI / 10.0f);
        if (i % 2 == 0)   // 바깥 꼭짓점
            pts[i] = { cosf(angle) * nOutX, sinf(angle) * nOutY };
        else              // 안쪽 꼭짓점
            pts[i] = { cosf(angle) * nInX,  sinf(angle) * nInY };
    }

    std::vector<Vertex> verts;
    verts.reserve(30);
    for (int i = 0; i < 10; ++i)
    {
        verts.push_back({ { 0, 0, 0 },                    { 1,1,1,1 } }); // 중심
        verts.push_back({ { pts[i].x, pts[i].y, 0 },      { 1,1,1,1 } });
        verts.push_back({ { pts[(i + 1) % 10].x, pts[(i + 1) % 10].y, 0 }, { 1,1,1,1 } });
    }

    Mesh* mesh = new Mesh();
    mesh->Create(device, verts);
    return mesh;
}

Mesh* CreateShipMesh(ID3D11Device* device, float r)
{
    float nW = (r / SCREEN_W) * 2.0f;
    float nH = (r / SCREEN_H) * 2.0f;

    std::vector<Vertex> verts;

    // 몸체
    verts.push_back({ {  0.0f,      nH * 2.0f, 0 }, { 1,1,1,1 } }); // 앞 꼭짓점 (위)
    verts.push_back({ {  nW,       -nH,        0 }, { 1,1,1,1 } }); // 오른쪽 아래
    verts.push_back({ { -nW,       -nH,        0 }, { 1,1,1,1 } }); // 왼쪽 아래

    // 왼쪽 날개 — 순서 수정
    verts.push_back({ {  0.0f,     -nH * 0.5f, 0 }, { 1,1,1,1 } }); // 날개 안쪽
    verts.push_back({ { -nW,       -nH,        0 }, { 1,1,1,1 } }); // 몸체 연결
    verts.push_back({ { -nW * 2.5f,-nH * 1.5f, 0 }, { 1,1,1,1 } }); // 날개 끝

    // 오른쪽 날개 — 순서 수정
    verts.push_back({ {  0.0f,     -nH * 0.5f, 0 }, { 1,1,1,1 } }); // 날개 안쪽
    verts.push_back({ {  nW * 2.5f,-nH * 1.5f, 0 }, { 1,1,1,1 } }); // 날개 끝
    verts.push_back({ {  nW,       -nH,        0 }, { 1,1,1,1 } }); // 몸체 연결

    Mesh* mesh = new Mesh();
    mesh->Create(device, verts);
    return mesh;
}

Mesh* CreateMeteorMesh(ID3D11Device* device, float r)
{
    const int POINTS = 8;

    // jitter 없이 순수 원형으로 먼저 테스트
    float nW = (r / SCREEN_W) * 2.0f;
    float nH = (r / SCREEN_H) * 2.0f;

    XMFLOAT2 pts[POINTS];
    for (int i = 0; i < POINTS; ++i)
    {
        float angle = (i * XM_2PI / POINTS);
        pts[i] = { cosf(angle) * nW, sinf(angle) * nH };
        printf("[Meteor] pt[%d] = (%.4f, %.4f)\n", i, pts[i].x, pts[i].y);
    }

    std::vector<Vertex> verts;
    verts.reserve(POINTS * 3);
    for (int i = 0; i < POINTS; ++i)
    {
        verts.push_back({ { 0, 0, 0 },                                      { 1,1,1,1 } }); 
        verts.push_back({ { pts[(i + 1) % POINTS].x, pts[(i + 1) % POINTS].y, 0 }, { 1,1,1,1 } });
        verts.push_back({ { pts[i].x, pts[i].y, 0 },                        { 1,1,1,1 } });
    }

    Mesh* mesh = new Mesh();
    mesh->Create(device, verts);
    return mesh;
}


// ============================================================
// WndProc / WinMain
// ============================================================
LRESULT CALLBACK GlobalWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hI, HINSTANCE, LPSTR, int)
{
    GameLoop gEngine;
    gEngine.Initialize(hI, GlobalWndProc);

    // ── InputLayout 정의 ───────────────────────────────────
    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    // ── 셰이더 한 번만 컴파일 & 생성 (강의: 재사용 원칙) ─
    ShaderSet shaders = gEngine.gfx.CompileAndCreate(SHADER_SRC, ied, 2);
    if (!shaders.vs || !shaders.ps)
    {
        printf("[Error] Shader compile failed.\n"); return -1;
    }

    // ── Mesh 생성 (강의: GPU 버퍼는 Mesh 가 소유) ─────────
    Mesh* playerMesh = CreateShipMesh(gEngine.gfx.Device, 15.0f);
    Mesh* obsMesh = CreateMeteorMesh(gEngine.gfx.Device, 15.0f);
    Mesh* itemMesh = CreateStarMesh(gEngine.gfx.Device, ITEM_HALF_W, ITEM_HALF_W * 0.42f);

    // ── Material 생성 (강의: 같은 ShaderSet 공유, 색상만 다름) ─
    // BlinkRenderer 가 색상을 실시간으로 바꾸므로 playerMat 는 ColorMaterial* 로 보관
    ColorMaterial* playerMat = new ColorMaterial(shaders, { 0.3f, 0.8f, 1.0f, 1.0f }, gEngine.gfx.Device); // 하늘색
    ColorMaterial* obsMat = new ColorMaterial(shaders, { 1.0f, 0.5f, 0.2f, 1.0f }, gEngine.gfx.Device); // 빨간색
    ColorMaterial* itemMat = new ColorMaterial(shaders, { 1.0f, 0.9f, 0.1f, 1.0f }, gEngine.gfx.Device); // 노란색

    // ── 플레이어 GameObject 조립 ──────────────────────────
    auto* player = new GameObject(SCREEN_W / 2.0f, SCREEN_H / 2.0f, PLAYER_HALF_W, PLAYER_HALF_H);
    player->AddComponent(new MeshRenderer(playerMesh, playerMat));
    player->AddComponent(new PlayerController());
    player->AddComponent(new BlinkRenderer(playerMat));         // 무적 깜빡임 (B 담당)
    player->AddComponent(new CooldownBarRenderer(gEngine.win.hWnd)); // 게이지 UI (B 담당)
    gEngine.world.push_back(player);

    // ── GameManager 연결 ───────────────────────────────────
    gEngine.manager.player = player;
    gEngine.manager.obstacles = &gEngine.obstacles;
    gEngine.manager.items = &gEngine.items;
    gEngine.manager.gfx = &gEngine.gfx;
    gEngine.manager.obsMesh = obsMesh;
    gEngine.manager.obsMat = obsMat;
    gEngine.manager.itemMesh = itemMesh;
    gEngine.manager.itemMat = itemMat;

    printf("[Game] 메인화면 | SPACE: 시작 / ESC: 종료\n");

    gEngine.Run();

    // ── 공유 리소스 해제 (소유자 WinMain 이 마지막에) ────
    delete playerMat;
    delete obsMat;
    delete itemMat;
    delete playerMesh;
    delete obsMesh;
    delete itemMesh;
    shaders.Release();

    return 0;
}
