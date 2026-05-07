/*
================================================================================
 Dodge++ (닷지없는 닷지게임)
 11주차 : 기본 게임 로직 완성
--------------------------------------------------------------------------------
 [11주차 추가 구현 목표]
 A (로직)
   1. 플레이어 이동 + 화면 경계 처리      → 10주차에서 이월 (유지)
   2. 장애물 생성 (랜덤 위치, 랜덤 속도)  → ObstacleController (신규)
   3. AABB 충돌 판정 구현                → GameManager::CheckCollision (신규)
   4. 점수 시스템 (생존 시간 기반)        → GameManager::score (신규)
   5. 게임오버 처리                      → GameManager::state (신규)
 B (렌더링)
   6. 메인화면 UI 렌더링                 → GameManager::RenderMain (신규)
   7. 게임오버 화면 렌더링               → GameManager::RenderGameOver (신규)
   8. 점수 텍스트 렌더링 (비트맵 폰트)   → TextRenderer (신규)
   9. deltaTime 기반 프레임 독립 처리    → 10주차에서 이월 (유지)
--------------------------------------------------------------------------------
 [11주차 완료 기준]
 - 충돌하면 게임오버, 점수가 올라가는 완전한 한 사이클
 - 메인화면(SPACE) → 게임중 → 게임오버(R 재시작) 흐름
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

#pragma comment(linker, "/entry:WinMainCRTStartup /subsystem:console")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// ============================================================
// 화면 / 플레이어 / 장애물 스펙 상수 (기획서 수치)
// ============================================================
static const int   SCREEN_W = 800;
static const int   SCREEN_H = 600;

// 플레이어
static const float PLAYER_SPEED = 500.0f;   // 픽셀/초
static const float PLAYER_HALF_W = 20.0f;
static const float PLAYER_HALF_H = 20.0f;

// 장애물
static const float OBS_HALF_W = 15.0f;    // 장애물 가로 절반
static const float OBS_HALF_H = 15.0f;    // 장애물 세로 절반
static const float OBS_SPEED_MIN = 150.0f;   // 최소 속도 (픽셀/초)
static const float OBS_SPEED_MAX = 350.0f;   // 최대 속도 (픽셀/초)
static const float OBS_SPAWN_INTERVAL = 1.2f;     // 스폰 간격 (초)
static const int   OBS_MAX = 30;        // 최대 장애물 수

// ============================================================
// 게임 상태
// ============================================================
enum class GameState
{
    MAIN,       // 메인화면 (SPACE로 시작)
    PLAYING,    // 게임 중
    GAMEOVER    // 게임오버 (R로 재시작)
};

// ============================================================
// Vertex / ConstantBuffer / Mesh (10주차와 동일)
// ============================================================
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT4 col;
};

struct ConstantBuffer
{
    XMMATRIX matWorld;
};

struct Mesh
{
    ID3D11Buffer* vBuffer = nullptr;
    ID3D11InputLayout* pInputLayout = nullptr;
    ID3D11VertexShader* pVS = nullptr;
    ID3D11PixelShader* pPS = nullptr;
    UINT                vertexCount = 0;
    XMFLOAT4            color = { 1, 1, 1, 1 };

    ~Mesh()
    {
        if (vBuffer)      vBuffer->Release();
        if (pInputLayout) pInputLayout->Release();
        if (pVS)          pVS->Release();
        if (pPS)          pPS->Release();
    }
};

// ============================================================
// DeltaTime (10주차와 동일)
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
        if (dt > 0.05f) dt = 0.05f;  // 최대 deltaTime 제한
        return dt;
    }
};

// ============================================================
// WindowContext (10주차와 동일, 타이틀만 변경)
// ============================================================
class WindowContext
{
public:
    HWND    hWnd = nullptr;
    int     Width = SCREEN_W;
    int     Height = SCREEN_H;

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

        hWnd = CreateWindow(
            L"DX11Engine", L"Dodge++ | Week 11",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            NULL, NULL, hInst, NULL
        );
        if (!hWnd) return false;

        ShowWindow(hWnd, SW_SHOW);
        printf("[Engine] Window Created. (%dx%d)\n", Width, Height);
        return true;
    }
};

// ============================================================
// GraphicsContext (10주차와 동일)
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
        sd.BufferDesc.Width = w;
        sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
            NULL, 0, D3D11_SDK_VERSION,
            &sd, &SwapChain, &Device, NULL, &ImmediateContext
        );
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

    ID3DBlob* CompileShader(const std::string& src, const std::string& entry, const std::string& profile)
    {
        ID3DBlob* blob = nullptr, * err = nullptr;
        D3DCompile(src.c_str(), src.length(), NULL, NULL, NULL,
            entry.c_str(), profile.c_str(), 0, 0, &blob, &err);
        if (err) { printf("[Shader Error] %s\n", (char*)err->GetBufferPointer()); err->Release(); }
        return blob;
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
// Component / GameObject (10주차와 동일)
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
    float halfW = 0.0f;   // 충돌 판정용 크기 (11주차 추가)
    float halfH = 0.0f;
    bool  active = true;
    std::vector<Component*> components;

    GameObject(float x, float y, float hw = 0, float hh = 0)
        : posX(x), posY(y), halfW(hw), halfH(hh) {
    }

    ~GameObject() { for (auto* c : components) delete c; }

    void AddComponent(Component* c) { c->pOwner = this; components.push_back(c); }

    void Input() { for (auto* c : components) if (c) c->Input(); }

    void Update(float dt, GraphicsContext* gfx)
    {
        for (auto* c : components)
        {
            if (!c) continue;
            if (!c->isStarted) { c->Start(gfx); c->isStarted = true; }
            c->Update(dt);
        }
    }

    void Render(GraphicsContext* gfx) { for (auto* c : components) if (c) c->Render(gfx); }
};

// ============================================================
// AABB 충돌 판정 (A 담당 - 11주차 핵심)
// ============================================================
bool CheckAABB(const GameObject& a, const GameObject& b)
{
    return (a.posX - a.halfW < b.posX + b.halfW &&
        a.posX + a.halfW > b.posX - b.halfW &&
        a.posY - a.halfH < b.posY + b.halfH &&
        a.posY + a.halfH > b.posY - b.halfH);
}

// ============================================================
// 전역 게임 상태 (GameManager에서 관리)
// ============================================================
GameState  gState = GameState::MAIN;
float      gScore = 0.0f;   // 생존 시간 = 점수
bool       gIsRunning = true;

// ============================================================
// MeshRenderer (10주차와 동일, 픽셀→NDC 변환 유지)
// ============================================================
class MeshRenderer : public Component
{
    Mesh* pMeshData = nullptr;
    ID3D11Buffer* cBuffer = nullptr;

public:
    MeshRenderer(Mesh* mesh) : pMeshData(mesh) {}
    ~MeshRenderer() { if (cBuffer) cBuffer->Release(); if (pMeshData) delete pMeshData; }

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
        if (!pMeshData || !pMeshData->vBuffer) return;

        float ndcX = (pOwner->posX / SCREEN_W) * 2.0f - 1.0f;
        float ndcY = 1.0f - (pOwner->posY / SCREEN_H) * 2.0f;

        XMMATRIX world = XMMatrixTranslation(ndcX, ndcY, 0.0f);
        ConstantBuffer cb;
        cb.matWorld = XMMatrixTranspose(world);

        gfx->ImmediateContext->IASetInputLayout(pMeshData->pInputLayout);
        gfx->ImmediateContext->VSSetShader(pMeshData->pVS, nullptr, 0);
        gfx->ImmediateContext->PSSetShader(pMeshData->pPS, nullptr, 0);
        gfx->ImmediateContext->UpdateSubresource(cBuffer, 0, nullptr, &cb, 0, 0);
        gfx->ImmediateContext->VSSetConstantBuffers(0, 1, &cBuffer);

        UINT stride = sizeof(Vertex), offset = 0;
        gfx->ImmediateContext->IASetVertexBuffers(0, 1, &pMeshData->vBuffer, &stride, &offset);
        gfx->ImmediateContext->Draw(pMeshData->vertexCount, 0);
    }

    void Input() override {}
    void Update(float dt) override {}
};

// ============================================================
// PlayerController (10주차와 동일)
// ============================================================
class PlayerController : public Component
{
    XMFLOAT2 moveDir = { 0, 0 };

public:
    void Start(GraphicsContext* gfx) override
    {
        printf("[Player] Started. 방향키 이동 | ESC 종료\n");
    }

    void Input() override
    {
        moveDir = { 0, 0 };
        // PLAYING 상태일 때만 입력 받음
        if (gState != GameState::PLAYING) return;

        if (GetAsyncKeyState(VK_UP) & 0x8000) moveDir.y -= 1.0f;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) moveDir.y += 1.0f;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) moveDir.x -= 1.0f;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) moveDir.x += 1.0f;
    }

    void Update(float dt) override
    {
        if (gState != GameState::PLAYING) return;

        pOwner->posX += moveDir.x * PLAYER_SPEED * dt;
        pOwner->posY += moveDir.y * PLAYER_SPEED * dt;

        // 화면 경계 처리
        if (pOwner->posX < PLAYER_HALF_W)              pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W)   pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)              pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H)   pOwner->posY = SCREEN_H - PLAYER_HALF_H;
    }

    void Render(GraphicsContext* gfx) override {}
};

// ============================================================
// ObstacleController (A 담당 - 11주차 신규)
// - 화면 위에서 아래로 낙하
// - 화면 밖으로 나가면 비활성화 (재사용 풀 방식)
// ============================================================
class ObstacleController : public Component
{
public:
    float velX = 0.0f;  // 가로 속도
    float velY = 0.0f;  // 세로 속도 (항상 양수 = 아래로)

    void Start(GraphicsContext* gfx) override {}

    void Input() override {}

    void Update(float dt) override
    {
        if (!pOwner->active) return;

        pOwner->posX += velX * dt;
        pOwner->posY += velY * dt;

        // 4방향 화면 밖으로 벗어나면 비활성화
        if (pOwner->posY > SCREEN_H + OBS_HALF_H * 2) pOwner->active = false;  // 아래
        if (pOwner->posY < -OBS_HALF_H * 2)           pOwner->active = false;  // 위
        if (pOwner->posX > SCREEN_W + OBS_HALF_W * 2) pOwner->active = false;  // 오른쪽
        if (pOwner->posX < -OBS_HALF_W * 2)           pOwner->active = false;  // 왼쪽
    }

    void Render(GraphicsContext* gfx) override {}
};

// ============================================================
// TextRenderer (B 담당 - 11주차 신규)
// Win32 GDI를 이용한 텍스트 출력
// DirectX 위에 GDI 오버레이 방식 사용
// ============================================================
class TextRenderer
{
    HWND  hWnd = nullptr;
    HFONT hFont = nullptr;
    HFONT hFontLarge = nullptr;

public:
    void Initialize(HWND hwnd)
    {
        hWnd = hwnd;

        // 기본 폰트 (점수 표시용)
        hFont = CreateFont(
            28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Consolas"
        );

        // 큰 폰트 (메인화면/게임오버 타이틀용)
        hFontLarge = CreateFont(
            52, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Consolas"
        );
    }

    // GDI 오버레이로 텍스트 출력
    void DrawText(const std::wstring& text, int x, int y, COLORREF color, bool large = false)
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
        if (gState == GameState::MAIN)
        {
            // ── 메인화면 ──────────────────────────────────
            DrawText(L"DODGE++", 280, 180, RGB(255, 255, 255), true);
            DrawText(L"닷지없는 닷지게임", 270, 250, RGB(180, 180, 255));
            DrawText(L"SPACE  :  게임 시작", 285, 340, RGB(200, 200, 200));
            DrawText(L"방향키 :  이동", 320, 380, RGB(200, 200, 200));
            DrawText(L"ESC    :  종료", 320, 420, RGB(200, 200, 200));
        }
        else if (gState == GameState::PLAYING)
        {
            // ── 게임 중: 점수 표시 ────────────────────────
            std::wstring scoreText = L"SCORE : " + std::to_wstring((int)gScore);
            DrawText(scoreText, 10, 10, RGB(255, 255, 100));
        }
        else if (gState == GameState::GAMEOVER)
        {
            // ── 게임오버 화면 ─────────────────────────────
            DrawText(L"GAME OVER", 295, 180, RGB(255, 80, 80), true);
            std::wstring scoreText = L"SCORE : " + std::to_wstring((int)gScore);
            DrawText(scoreText, 330, 270, RGB(255, 255, 100));
            DrawText(L"R  :  재시작", 330, 340, RGB(200, 200, 200));
            DrawText(L"ESC  :  종료", 330, 380, RGB(200, 200, 200));
        }
    }

    ~TextRenderer()
    {
        if (hFont)      DeleteObject(hFont);
        if (hFontLarge) DeleteObject(hFontLarge);
    }
};

// ============================================================
// GameManager (A 담당 - 11주차 신규)
// - 게임 상태 전환 (MAIN → PLAYING → GAMEOVER)
// - 장애물 스폰 관리
// - 충돌 판정
// - 점수 관리
// ============================================================
class GameManager
{
    std::mt19937                          rng;
    std::uniform_real_distribution<float> randPosX;   // 장애물 X 스폰 위치
    std::uniform_real_distribution<float> randPosY;   // 장애물 Y 스폰 위치
    std::uniform_real_distribution<float> randSpeed;  // 장애물 속도 크기
    std::uniform_int_distribution<int>    randSide;   // 0=위 1=아래 2=왼 3=오른

    float spawnTimer = 0.0f;

public:
    std::vector<GameObject*>* obstacles = nullptr;
    GameObject* player = nullptr;
    GraphicsContext* gfx = nullptr;
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;

    GameManager()
        : rng(std::random_device{}())
        , randPosX(OBS_HALF_W, SCREEN_W - OBS_HALF_W)
        , randPosY(OBS_HALF_H, SCREEN_H - OBS_HALF_H)
        , randSpeed(OBS_SPEED_MIN, OBS_SPEED_MAX)
        , randSide(0, 3)
    {
    }

    // ── 4방향 스폰 위치/속도 계산 헬퍼 ──────────────────────
    // side: 0=위, 1=아래, 2=왼쪽, 3=오른쪽
    // 메인 방향 속도 + 랜덤 사선 성분으로 대각선 이동 가능
    void CalcSpawn(float& outX, float& outY, float& outVelX, float& outVelY)
    {
        int   side = randSide(rng);
        float speed = randSpeed(rng);

        // 사선 편차: -0.7 ~ 0.7 범위
        // 0이면 직선, 클수록 더 대각선으로 날아옴
        std::uniform_real_distribution<float> randDiag(-1.0f, 1.0f);
        float diag = randDiag(rng);

        switch (side)
        {
        case 0: // 위 → 아래로 낙하 (좌우 사선 추가)
            outX = randPosX(rng);
            outY = -OBS_HALF_H;
            outVelX = speed * diag;
            outVelY = speed;
            break;
        case 1: // 아래 → 위로 올라감 (좌우 사선 추가)
            outX = randPosX(rng);
            outY = SCREEN_H + OBS_HALF_H;
            outVelX = speed * diag;
            outVelY = -speed;
            break;
        case 2: // 왼쪽 → 오른쪽 (상하 사선 추가)
            outX = -OBS_HALF_W;
            outY = randPosY(rng);
            outVelX = speed;
            outVelY = speed * diag;
            break;
        case 3: // 오른쪽 → 왼쪽 (상하 사선 추가)
            outX = SCREEN_W + OBS_HALF_W;
            outY = randPosY(rng);
            outVelX = -speed;
            outVelY = speed * diag;
            break;
        }
    }

    // vsBlob/psBlob은 GameManager가 끝까지 들고 있다가 소멸 시 해제
    ~GameManager()
    {
        if (vsBlob) { vsBlob->Release(); vsBlob = nullptr; }
        if (psBlob) { psBlob->Release(); psBlob = nullptr; }
    }

    // 게임 재시작 (점수 초기화, 플레이어 위치 초기화, 장애물 전부 비활성화)
    void ResetGame()
    {
        gScore = 0.0f;
        spawnTimer = 0.0f;

        // 플레이어 중앙 복귀
        if (player)
        {
            player->posX = SCREEN_W / 2.0f;
            player->posY = SCREEN_H / 2.0f;
            player->active = true;
        }

        // 장애물 전부 비활성화
        if (obstacles)
        {
            for (auto* obj : *obstacles)
                obj->active = false;
        }

        printf("[Game] Reset. PLAYING 시작!\n");
    }

    // 장애물 스폰 (오브젝트 풀 방식: 비활성 재사용 → 없으면 새로 생성)
    void SpawnObstacle()
    {
        if (!obstacles || !gfx) return;

        // 비활성 장애물 재사용 시도
        for (auto* obj : *obstacles)
        {
            if (!obj->active)
            {
                float spawnX, spawnY, velX, velY;
                CalcSpawn(spawnX, spawnY, velX, velY);

                obj->posX = spawnX;
                obj->posY = spawnY;
                obj->active = true;

                // 속도 재설정
                auto* ctrl = dynamic_cast<ObstacleController*>(obj->components[0]);
                if (ctrl)
                {
                    ctrl->velX = velX;
                    ctrl->velY = velY;
                }
                return;
            }
        }

        // 최대 개수 미만이면 새 장애물 생성
        if ((int)obstacles->size() >= OBS_MAX) return;

        // 장애물 Mesh 생성
        float nW = (OBS_HALF_W / SCREEN_W) * 2.0f;
        float nH = (OBS_HALF_H / SCREEN_H) * 2.0f;

        Vertex verts[6] = {
            { { -nW,  nH, 0 }, { 1.0f, 0.3f, 0.3f, 1.0f } },
            { {  nW,  nH, 0 }, { 1.0f, 0.3f, 0.3f, 1.0f } },
            { {  nW, -nH, 0 }, { 1.0f, 0.3f, 0.3f, 1.0f } },
            { { -nW,  nH, 0 }, { 1.0f, 0.3f, 0.3f, 1.0f } },
            { {  nW, -nH, 0 }, { 1.0f, 0.3f, 0.3f, 1.0f } },
            { { -nW, -nH, 0 }, { 1.0f, 0.3f, 0.3f, 1.0f } },
        };

        Mesh* mesh = new Mesh();
        mesh->vertexCount = 6;
        mesh->color = { 1.0f, 0.3f, 0.3f, 1.0f };  // 빨간색

        gfx->Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &mesh->pVS);
        gfx->Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &mesh->pPS);

        D3D11_INPUT_ELEMENT_DESC ied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        };
        gfx->Device->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &mesh->pInputLayout);

        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = sizeof(verts);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd2 = { verts };
        gfx->Device->CreateBuffer(&bd, &sd2, &mesh->vBuffer);

        // 장애물 오브젝트 생성
        float spawnX, spawnY, velX, velY;
        CalcSpawn(spawnX, spawnY, velX, velY);

        auto* obs = new GameObject(spawnX, spawnY, OBS_HALF_W, OBS_HALF_H);
        auto* ctrl = new ObstacleController();
        ctrl->velX = velX;
        ctrl->velY = velY;

        obs->AddComponent(ctrl);
        obs->AddComponent(new MeshRenderer(mesh));
        obstacles->push_back(obs);
    }

    // 매 프레임 호출
    void Update(float dt)
    {
        if (gState == GameState::PLAYING)
        {
            // 점수 = 생존 시간 (초)
            gScore += dt;

            // 장애물 스폰 타이머
            spawnTimer += dt;
            if (spawnTimer >= OBS_SPAWN_INTERVAL)
            {
                SpawnObstacle();
                spawnTimer = 0.0f;
            }

            // 충돌 판정
            if (player && player->active && obstacles)
            {
                for (auto* obs : *obstacles)
                {
                    if (!obs->active) continue;
                    if (CheckAABB(*player, *obs))
                    {
                        // 충돌 → 게임오버
                        gState = GameState::GAMEOVER;
                        printf("[Game] GAME OVER! Score: %.1f\n", gScore);
                        break;
                    }
                }
            }
        }
    }
};

// ============================================================
// GameLoop (10주차 확장)
// ============================================================
class GameLoop
{
public:
    WindowContext             win;
    GraphicsContext           gfx;
    DeltaTime                 timer;
    std::vector<GameObject*>  world;       // 플레이어 포함
    std::vector<GameObject*>  obstacles;   // 장애물 풀
    TextRenderer              text;
    GameManager               manager;
    bool                      isRunning = true;

    GameLoop() { printf("[Engine] GameLoop Created.\n"); }

    ~GameLoop()
    {
        for (auto* obj : world)     delete obj;
        for (auto* obj : obstacles) delete obj;
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
        // ESC → 종료
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            isRunning = false;

        // SPACE → 메인화면에서 게임 시작
        if (gState == GameState::MAIN && (GetAsyncKeyState(VK_SPACE) & 0x0001))
        {
            gState = GameState::PLAYING;
            manager.ResetGame();
        }

        // R → 게임오버에서 재시작
        if (gState == GameState::GAMEOVER && (GetAsyncKeyState('R') & 0x0001))
        {
            gState = GameState::PLAYING;
            manager.ResetGame();
        }

        // 오브젝트 입력 전파
        for (auto* obj : world)
            if (obj && obj->active) obj->Input();
    }

    void Update()
    {
        float dt = timer.GetDelta();

        // GameManager 업데이트 (스폰, 충돌, 점수)
        manager.Update(dt);

        // 플레이어 업데이트
        for (auto* obj : world)
            if (obj && obj->active) obj->Update(dt, &gfx);

        // 장애물 업데이트
        for (auto* obs : obstacles)
            if (obs && obs->active) obs->Update(dt, &gfx);
    }

    void Render()
    {
        // 배경 클리어 (짙은 남색)
        float col[] = { 0.05f, 0.05f, 0.15f, 1.0f };
        gfx.ImmediateContext->ClearRenderTargetView(gfx.RTV, col);

        D3D11_VIEWPORT vp = { 0, 0, (float)SCREEN_W, (float)SCREEN_H, 0, 1 };
        gfx.ImmediateContext->RSSetViewports(1, &vp);
        gfx.ImmediateContext->OMSetRenderTargets(1, &gfx.RTV, NULL);
        gfx.ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 장애물 렌더
        for (auto* obs : obstacles)
            if (obs && obs->active) obs->Render(&gfx);

        // 플레이어 렌더 (장애물 위에 그려짐)
        for (auto* obj : world)
            if (obj && obj->active) obj->Render(&gfx);

        gfx.SwapChain->Present(gfx.VSync, 0);

        // GDI 텍스트 오버레이 (Present 이후)
        text.Render();
    }

    void Run()
    {
        MSG msg = {};
        while (msg.message != WM_QUIT && isRunning)
        {
            if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            else
            {
                Input();
                Update();
                Render();
            }
        }
    }
};

// ============================================================
// 셰이더 소스 (10주차와 동일)
// ============================================================
static const std::string SHADER_SRC = R"(
    cbuffer cb0 : register(b0) { matrix matWorld; };

    struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };
    struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };

    PS_IN VS(VS_IN input)
    {
        PS_IN output;
        output.pos = mul(float4(input.pos, 1.0f), matWorld);
        output.col = input.col;
        return output;
    }

    float4 PS(PS_IN input) : SV_Target { return input.col; }
)";

// ============================================================
// 사각형 Mesh 생성 헬퍼 (10주차와 동일)
// ============================================================
Mesh* CreateRectMesh(
    GraphicsContext& gfx,
    ID3DBlob* vsBlob, ID3DBlob* psBlob,
    float halfW, float halfH,
    XMFLOAT4 color)
{
    float nW = (halfW / SCREEN_W) * 2.0f;
    float nH = (halfH / SCREEN_H) * 2.0f;

    Vertex verts[6] = {
        { { -nW,  nH, 0 }, color },
        { {  nW,  nH, 0 }, color },
        { {  nW, -nH, 0 }, color },
        { { -nW,  nH, 0 }, color },
        { {  nW, -nH, 0 }, color },
        { { -nW, -nH, 0 }, color },
    };

    Mesh* mesh = new Mesh();
    mesh->color = color;
    mesh->vertexCount = 6;

    gfx.Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &mesh->pVS);
    gfx.Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &mesh->pPS);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    gfx.Device->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &mesh->pInputLayout);

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(verts);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = { verts };
    gfx.Device->CreateBuffer(&bd, &sd, &mesh->vBuffer);

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

    // 셰이더 컴파일
    ID3DBlob* vsBlob = gEngine.gfx.CompileShader(SHADER_SRC, "VS", "vs_5_0");
    ID3DBlob* psBlob = gEngine.gfx.CompileShader(SHADER_SRC, "PS", "ps_5_0");
    if (!vsBlob || !psBlob) { printf("[Error] Shader compile failed.\n"); return -1; }

    // ── 플레이어 생성 (흰 사각형, 화면 중앙) ──────────────
    Mesh* playerMesh = CreateRectMesh(
        gEngine.gfx, vsBlob, psBlob,
        PLAYER_HALF_W, PLAYER_HALF_H,
        { 1.0f, 1.0f, 1.0f, 1.0f }   // 흰색
    );

    GameObject* player = new GameObject(
        SCREEN_W / 2.0f, SCREEN_H / 2.0f,
        PLAYER_HALF_W, PLAYER_HALF_H   // 충돌 판정 크기 등록
    );
    player->AddComponent(new MeshRenderer(playerMesh));
    player->AddComponent(new PlayerController());
    gEngine.world.push_back(player);

    // ── GameManager 연결 ───────────────────────────────────
    gEngine.manager.player = player;
    gEngine.manager.obstacles = &gEngine.obstacles;
    gEngine.manager.gfx = &gEngine.gfx;
    // vsBlob/psBlob은 GameManager 소멸자에서 해제됨 (여기서 Release 하면 안 됨!)
    gEngine.manager.vsBlob = vsBlob;
    gEngine.manager.psBlob = psBlob;

    printf("[Game] 메인화면 | SPACE: 시작 / ESC: 종료\n");

    gEngine.Run();

    return 0;
}
