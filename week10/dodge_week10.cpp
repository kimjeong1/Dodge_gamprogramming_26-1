/*
================================================================================
 Dodge++ (닷지없는 닷지게임)
 10주차 : 환경 세팅 + 플레이어 기본 이동
--------------------------------------------------------------------------------
 [10주차 구현 목표]
 1. WindowContext  : Win32 창 생성 및 메시지 루프 관리
 2. GraphicsContext: DX11 디바이스, 스왑체인, 셰이더 초기화
 3. DeltaTime      : 고해상도 타이머를 이용한 시간 계산
 4. GameObject     : 객체 지향적 기능 확장 구조 (Component 패턴 유지)
 5. PlayerController : 방향키 이동 + 화면 경계 처리
--------------------------------------------------------------------------------
 [10주차 완료 기준]
 - 흰 사각형(플레이어)이 방향키로 움직이는 상태
 - 화면 밖으로 나가지 않는 경계 처리 완료
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

#pragma comment(linker, "/entry:WinMainCRTStartup /subsystem:console")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace DirectX;

// ============================================================
// 화면 크기 상수 (기획서 수치)
// ============================================================
static const int SCREEN_W = 800;
static const int SCREEN_H = 600;

// ============================================================
// 플레이어 스펙 상수 (기획서 수치)
// ============================================================
static const float PLAYER_SPEED = 500.0f;  // 픽셀/초
static const float PLAYER_HALF_W = 20.0f;   // 플레이어 가로 절반 크기
static const float PLAYER_HALF_H = 20.0f;   // 플레이어 세로 절반 크기

// ============================================================
// Vertex / ConstantBuffer 구조체
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

// ============================================================
// Mesh : GPU 리소스 묶음
// ============================================================
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
// DeltaTime
// ============================================================
class DeltaTime
{
    std::chrono::high_resolution_clock::time_point prevTime;
public:
    DeltaTime()
    {
        prevTime = std::chrono::high_resolution_clock::now();
    }

    float GetDelta()
    {
        auto currTime = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(currTime - prevTime).count();
        prevTime = currTime;
        // 최대 deltaTime 제한 (창 이동 등으로 튀는 현상 방지)
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
    HWND     hWnd = nullptr;
    int      Width = SCREEN_W;
    int      Height = SCREEN_H;
    LPCWSTR  windowName = L"Dodge++ | Week 10";

    ~WindowContext()
    {
        UnregisterClass(L"DX11Engine", GetModuleHandle(NULL));
    }

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
            L"DX11Engine", windowName,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
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

        if (FAILED(hr))
        {
            printf("[Error] D3D11CreateDeviceAndSwapChain failed.\n");
            return false;
        }

        CreateRTV(w, h);
        printf("[Engine] DirectX 11 Initialized.\n");
        return true;
    }

    void CreateRTV(int w, int h)
    {
        if (RTV) RTV->Release();
        ID3D11Texture2D* pBB = nullptr;
        SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBB);
        Device->CreateRenderTargetView(pBB, NULL, &RTV);
        pBB->Release();
    }

    // 셰이더 소스 문자열 컴파일
    ID3DBlob* CompileShader(const std::string& src, const std::string& entry, const std::string& profile)
    {
        ID3DBlob* blob = nullptr;
        ID3DBlob* error = nullptr;
        HRESULT hr = D3DCompile(
            src.c_str(), src.length(), NULL, NULL, NULL,
            entry.c_str(), profile.c_str(), 0, 0, &blob, &error
        );
        if (FAILED(hr) && error)
        {
            printf("[Shader Error] %s\n", (char*)error->GetBufferPointer());
            error->Release();
        }
        return blob;
    }

    ~GraphicsContext()
    {
        if (RTV)             RTV->Release();
        if (SwapChain)       SwapChain->Release();
        if (ImmediateContext) ImmediateContext->Release();
        if (Device)          Device->Release();
    }
};

// ============================================================
// Component / GameObject (원본 구조 유지)
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
    // NDC(-1~1) 좌표계 대신 픽셀 좌표계 사용 (직관적)
    float posX = 0.0f;  // 픽셀 X (0 = 화면 왼쪽)
    float posY = 0.0f;  // 픽셀 Y (0 = 화면 위쪽)
    bool  active = true;

    std::vector<Component*> components;

    GameObject(float x, float y) : posX(x), posY(y) {}

    ~GameObject()
    {
        for (auto* c : components) delete c;
    }

    void AddComponent(Component* c)
    {
        c->pOwner = this;
        components.push_back(c);
    }

    void Input()
    {
        for (auto* c : components)
            if (c) c->Input();
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
        for (auto* c : components)
            if (c) c->Render(gfx);
    }
};

// ============================================================
// MeshRenderer (픽셀 좌표 → NDC 변환 포함)
// ============================================================
class MeshRenderer : public Component
{
    Mesh* pMeshData = nullptr;
    ID3D11Buffer* cBuffer = nullptr;

public:
    MeshRenderer(Mesh* mesh) : pMeshData(mesh) {}

    ~MeshRenderer()
    {
        if (cBuffer)   cBuffer->Release();
        if (pMeshData) delete pMeshData;
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
        if (!pMeshData || !pMeshData->vBuffer) return;

        // 픽셀 좌표 → NDC 변환
        // NDC x = (pixelX / screenW) * 2 - 1
        // NDC y = 1 - (pixelY / screenH) * 2
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

    void Input()  override {}
    void Update(float dt) override {}
};

// ============================================================
// PlayerController (10주차 핵심 구현)
// - 방향키 이동
// - 화면 경계 처리
// ============================================================
class PlayerController : public Component
{
    XMFLOAT2 moveDir = { 0, 0 };  // 이동 방향 벡터

public:
    void Start(GraphicsContext* gfx) override
    {
        printf("[Player] PlayerController Started.\n");
        printf("[Player] 방향키로 이동 | ESC 종료\n");
    }

    // [Step 1] 입력 감지 → moveDir 저장
    void Input() override
    {
        moveDir = { 0, 0 };

        if (GetAsyncKeyState(VK_UP) & 0x8000) moveDir.y -= 1.0f;  // 위 (픽셀Y 감소)
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) moveDir.y += 1.0f;  // 아래
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) moveDir.x -= 1.0f;  // 왼쪽
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) moveDir.x += 1.0f;  // 오른쪽
    }

    // [Step 2] moveDir 기반 위치 갱신 + 경계 처리
    void Update(float dt) override
    {
        // 이동 적용
        pOwner->posX += moveDir.x * PLAYER_SPEED * dt;
        pOwner->posY += moveDir.y * PLAYER_SPEED * dt;

        // ── 화면 경계 처리 ──────────────────────────────
        // 왼쪽/오른쪽 경계
        if (pOwner->posX < PLAYER_HALF_W)
            pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W)
            pOwner->posX = SCREEN_W - PLAYER_HALF_W;

        // 위쪽/아래쪽 경계
        if (pOwner->posY < PLAYER_HALF_H)
            pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H)
            pOwner->posY = SCREEN_H - PLAYER_HALF_H;
    }

    void Render(GraphicsContext* gfx) override {}
};

// ============================================================
// GameLoop
// ============================================================
class GameLoop
{
public:
    WindowContext           win;
    GraphicsContext         gfx;
    DeltaTime               timer;
    std::vector<GameObject*> world;
    bool                    isRunning = true;

    GameLoop()
    {
        printf("[Engine] GameLoop Created.\n");
    }

    ~GameLoop()
    {
        for (auto* obj : world) delete obj;
        world.clear();
        printf("[Engine] GameLoop Destroyed.\n");
    }

    void Initialize(HINSTANCE hInst, LRESULT(CALLBACK* wndProc)(HWND, UINT, WPARAM, LPARAM))
    {
        win.Initialize(hInst, wndProc);
        gfx.InitDX(win.hWnd, SCREEN_W, SCREEN_H);
    }

    void Input()
    {
        // ESC → 종료
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
            isRunning = false;

        for (auto* obj : world)
            if (obj) obj->Input();
    }

    void Update()
    {
        float dt = timer.GetDelta();
        for (auto* obj : world)
            if (obj) obj->Update(dt, &gfx);
    }

    void Render()
    {
        // 배경색: 우주 느낌의 짙은 남색
        float col[] = { 0.05f, 0.05f, 0.15f, 1.0f };
        gfx.ImmediateContext->ClearRenderTargetView(gfx.RTV, col);

        D3D11_VIEWPORT vp = { 0, 0, (float)SCREEN_W, (float)SCREEN_H, 0, 1 };
        gfx.ImmediateContext->RSSetViewports(1, &vp);
        gfx.ImmediateContext->OMSetRenderTargets(1, &gfx.RTV, NULL);
        gfx.ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (auto* obj : world)
            if (obj) obj->Render(&gfx);

        gfx.SwapChain->Present(gfx.VSync, 0);
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
// 셰이더 소스 (원본 유지)
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
// 사각형 Mesh 생성 헬퍼
// NDC 기준 크기: halfW, halfH (화면 비율 맞게 계산)
// ============================================================
Mesh* CreateRectMesh(
    GraphicsContext& gfx,
    ID3DBlob* vsBlob, ID3DBlob* psBlob,
    float halfW, float halfH,
    XMFLOAT4 color)
{
    // NDC 기준 크기 변환
    float nW = (halfW / SCREEN_W) * 2.0f;
    float nH = (halfH / SCREEN_H) * 2.0f;

    // 사각형 = 삼각형 2개 (6 vertex)
    Vertex verts[6] = {
        { { -nW,  nH, 0 }, color },  // 좌상
        { {  nW,  nH, 0 }, color },  // 우상
        { {  nW, -nH, 0 }, color },  // 우하
        { { -nW,  nH, 0 }, color },  // 좌상
        { {  nW, -nH, 0 }, color },  // 우하
        { { -nW, -nH, 0 }, color },  // 좌하
    };

    Mesh* mesh = new Mesh();
    mesh->color = color;
    mesh->vertexCount = 6;

    // VS / PS 생성
    gfx.Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &mesh->pVS);
    gfx.Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &mesh->pPS);

    // InputLayout
    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    gfx.Device->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &mesh->pInputLayout);

    // Vertex Buffer
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

    if (!vsBlob || !psBlob)
    {
        printf("[Error] Shader compile failed.\n");
        return -1;
    }

    // ──────────────────────────────────────────────────────
    // 플레이어 생성
    // 시작 위치: 화면 중앙
    // 색상: 흰색
    // 크기: 40x40 픽셀 (PLAYER_HALF_W/H = 20)
    // ──────────────────────────────────────────────────────
    Mesh* playerMesh = CreateRectMesh(
        gEngine.gfx, vsBlob, psBlob,
        PLAYER_HALF_W, PLAYER_HALF_H,
        { 1.0f, 1.0f, 1.0f, 1.0f }  // 흰색
    );

    GameObject* player = new GameObject(
        SCREEN_W / 2.0f,   // 중앙 X
        SCREEN_H / 2.0f    // 중앙 Y
    );
    player->AddComponent(new MeshRenderer(playerMesh));
    player->AddComponent(new PlayerController());

    gEngine.world.push_back(player);

    vsBlob->Release();
    psBlob->Release();

    printf("[Game] Start! 방향키로 이동하세요.\n");
    printf("[Game] 화면 경계를 벗어날 수 없습니다.\n");

    gEngine.Run();

    return 0;
}
