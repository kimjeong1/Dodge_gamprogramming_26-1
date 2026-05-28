/*
================================================================================
 Dodge++
 12주차 : 차별화 — 대시 스킬 + 무적 아이템
--------------------------------------------------------------------------------
 [GDI → DirectX 마이그레이션]
  - DashTrailRenderer  : GDI Polygon/FillRect → DynamicMesh (매 프레임 CPU→GPU 업로드)
  - CooldownBarRenderer: GDI FillRect/TextOut → DirectX 사각형 (TRIANGLELIST) +
                         LINELIST 외곽선 / 텍스트는 GDI 유지(별도 요청 전까지)
  - ShieldRenderer     : GDI Polyline → LINELIST
  - TextRenderer 별 배경: GDI SetPixel → DirectX 사각형(TRIANGLELIST)
  - TextRenderer 텍스트: GDI 유지 (폰트 미구현)

 렌더링 구조 변경 사항
  ┌─ PrimitiveBatch (신규) ────────────────────────────────────────────┐
  │  • CPU 버텍스 배열을 매 프레임 Dynamic Buffer 에 Map/Unmap         │
  │  • TRIANGLELIST / LINELIST 두 토폴로지 지원                        │
  │  • Begin() → AddTri() / AddLine() → End(topology) 패턴           │
  └────────────────────────────────────────────────────────────────────┘

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
#include <algorithm>

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

static const float PLAYER_SPEED = 500.0f;
static const float PLAYER_HALF_W = 12.0f;
static const float PLAYER_HALF_H = 12.0f;

static const float DASH_DISTANCE = 180.0f;
static const float DASH_COOLDOWN = 3.0f;

static const float OBS_HALF_W = 9.0f;
static const float OBS_HALF_H = 9.0f;
static const float OBS_SPEED_MIN = 150.0f;
static const float OBS_SPEED_MAX = 380.0f;
static const float OBS_SPAWN_INTERVAL = 1.2f;
static const int   OBS_MAX = 500;

static const float ITEM_HALF_W = 14.0f;
static const float ITEM_HALF_H = 14.0f;
static const float ITEM_SPAWN_INTERVAL = 15.0f;
static const float INVINCIBLE_DURATION = 5.0f;

static const int   GAUGE_X = 20;
static const int   GAUGE_Y = SCREEN_H - 40;
static const int   GAUGE_W = 160;
static const int   GAUGE_H = 18;

static const float DIFFICULTY_INTERVAL = 20.0f;
static const int   DIFFICULTY_MAX = 6;

// ============================================================
// 전역 게임 상태
// ============================================================
enum class GameState { MAIN, PLAYING, GAMEOVER };
GameState gState = GameState::MAIN;
float     gScore = 0.0f;
bool      gIsInvincible = false;
float     gInvTimer = 0.0f;
float     gDashCooldown = 0.0f;
float     gHighScore = 0.0f;

// ============================================================
// 기본 구조체
// ============================================================
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT4 col;
};

struct ConstantBuffer { XMMATRIX matWorld; };
struct ColorBuffer { XMFLOAT4 tintColor; };

// ============================================================
// NDC 변환 헬퍼 (픽셀 → NDC)
// ============================================================
static inline float PxToNdcX(float px) { return (px / SCREEN_W) * 2.0f - 1.0f; }
static inline float PxToNdcY(float py) { return 1.0f - (py / SCREEN_H) * 2.0f; }

// ============================================================
// ShaderSet
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
// Mesh (정적 GPU 버퍼 소유)
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
// PrimitiveBatch (신규)
// 매 프레임 CPU에서 버텍스를 쌓아 Dynamic Buffer 로 업로드
// TRIANGLELIST / LINELIST 두 토폴로지를 각각 한 번씩 Flush
// ============================================================
class PrimitiveBatch
{
    static const UINT MAX_VERTS = 65536;

    ID3D11Buffer* dynBuf = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* cbWorld = nullptr;   // 항등 행렬 고정
    ID3D11Buffer* cbColor = nullptr;   // 흰색 (버텍스 색 패스스루)

    std::vector<Vertex>  triVerts;   // TRIANGLELIST 용
    std::vector<Vertex>  lineVerts;  // LINELIST 용

public:
    bool Init(ID3D11Device* device, ShaderSet shaders)
    {
        vs = shaders.vs;
        ps = shaders.ps;
        layout = shaders.layout;

        // Dynamic vertex buffer
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(Vertex) * MAX_VERTS;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &dynBuf))) return false;

        // World CB (항등 행렬, 한 번만 씀)
        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(ConstantBuffer);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ConstantBuffer identity;
        identity.matWorld = XMMatrixTranspose(XMMatrixIdentity());
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = &identity;
        device->CreateBuffer(&cbd, &sd, &cbWorld);

        // Color CB (흰색: tintColor = 1,1,1,1 → 버텍스 색 그대로)
        cbd.ByteWidth = sizeof(ColorBuffer);
        ColorBuffer white = { {1,1,1,1} };
        sd.pSysMem = &white;
        device->CreateBuffer(&cbd, &sd, &cbColor);

        triVerts.reserve(4096);
        lineVerts.reserve(4096);
        return true;
    }

    ~PrimitiveBatch()
    {
        if (dynBuf) { dynBuf->Release();  dynBuf = nullptr; }
        if (cbWorld) { cbWorld->Release(); cbWorld = nullptr; }
        if (cbColor) { cbColor->Release(); cbColor = nullptr; }
        // vs/ps/layout 는 ShaderSet 이 소유 → 해제 안 함
    }

    // 삼각형 추가 (픽셀 좌표)
    void AddTri(XMFLOAT2 a, XMFLOAT2 b, XMFLOAT2 c, XMFLOAT4 col)
    {
        if (triVerts.size() + 3 > MAX_VERTS) return;
        triVerts.push_back({ { PxToNdcX(a.x), PxToNdcY(a.y), 0 }, col });
        triVerts.push_back({ { PxToNdcX(b.x), PxToNdcY(b.y), 0 }, col });
        triVerts.push_back({ { PxToNdcX(c.x), PxToNdcY(c.y), 0 }, col });
    }

    // 사각형 추가 (픽셀 좌표, 2삼각형)
    void AddRect(float x0, float y0, float x1, float y1, XMFLOAT4 col)
    {
        AddTri({ x0,y0 }, { x1,y0 }, { x1,y1 }, col);
        AddTri({ x0,y0 }, { x1,y1 }, { x0,y1 }, col);
    }

    // 라인 추가 (픽셀 좌표)
    void AddLine(XMFLOAT2 a, XMFLOAT2 b, XMFLOAT4 col)
    {
        if (lineVerts.size() + 2 > MAX_VERTS) return;
        lineVerts.push_back({ { PxToNdcX(a.x), PxToNdcY(a.y), 0 }, col });
        lineVerts.push_back({ { PxToNdcX(b.x), PxToNdcY(b.y), 0 }, col });
    }

    // 다각형 외곽선 (픽셀 좌표 배열, 닫힘)
    void AddPolyline(const std::vector<XMFLOAT2>& pts, XMFLOAT4 col, bool close = true)
    {
        int n = (int)pts.size();
        for (int i = 0; i < n - 1; ++i)
            AddLine(pts[i], pts[i + 1], col);
        if (close && n > 1)
            AddLine(pts[n - 1], pts[0], col);
    }

    // 매 프레임 말미에 일괄 제출
    void Flush(ID3D11DeviceContext* ctx)
    {
        if (triVerts.empty() && lineVerts.empty()) return;

        ctx->IASetInputLayout(layout);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cbWorld);
        ctx->PSSetConstantBuffers(1, 1, &cbColor);

        UINT stride = sizeof(Vertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &dynBuf, &stride, &offset);

        // ── TRIANGLELIST ──────────────────────────────────
        if (!triVerts.empty())
        {
            Upload(ctx, triVerts);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->Draw((UINT)triVerts.size(), 0);
            triVerts.clear();
        }

        // ── LINELIST ──────────────────────────────────────
        if (!lineVerts.empty())
        {
            Upload(ctx, lineVerts);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            ctx->Draw((UINT)lineVerts.size(), 0);
            lineVerts.clear();
        }

        // 다음 Render 패스가 TRIANGLELIST 를 기본으로 쓸 수 있도록 복원
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

private:
    void Upload(ID3D11DeviceContext* ctx, const std::vector<Vertex>& verts)
    {
        D3D11_MAPPED_SUBRESOURCE ms = {};
        if (SUCCEEDED(ctx->Map(dynBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        {
            memcpy(ms.pData, verts.data(), sizeof(Vertex) * verts.size());
            ctx->Unmap(dynBuf, 0);
        }
    }
};

// ============================================================
// Material (추상)
// ============================================================
class Material
{
public:
    ShaderSet shaders;
    Material(ShaderSet s) : shaders(s) {}
    virtual ~Material() {}
    virtual void Bind(ID3D11DeviceContext* context) = 0;
};

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
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT,
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

    // PrimitiveBatch: GDI 대체 렌더러 (전역 접근용)
    PrimitiveBatch          Batch;

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
        ShaderSet   res;
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
        : posX(x), posY(y), halfW(hw), halfH(hh) {}

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

bool CheckAABB(const GameObject& a, const GameObject& b)
{
    return (a.posX - a.halfW < b.posX + b.halfW &&
        a.posX + a.halfW > b.posX - b.halfW &&
        a.posY - a.halfH < b.posY + b.halfH &&
        a.posY + a.halfH > b.posY - b.halfH);
}

// ============================================================
// MeshRenderer (정적 Mesh 용, 기존 유지)
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

        float ndcX = PxToNdcX(pOwner->posX);
        float ndcY = PxToNdcY(pOwner->posY);

        XMMATRIX world = XMMatrixIdentity();
        world.r[3] = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
        ConstantBuffer cb;
        cb.matWorld = XMMatrixTranspose(world);
        gfx->ImmediateContext->UpdateSubresource(cBuffer, 0, nullptr, &cb, 0, 0);
        gfx->ImmediateContext->VSSetConstantBuffers(0, 1, &cBuffer);

        UINT stride = sizeof(Vertex), offset = 0;
        gfx->ImmediateContext->IASetVertexBuffers(0, 1, &pMeshData->vBuffer, &stride, &offset);
        gfx->ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gfx->ImmediateContext->Draw(pMeshData->vertexCount, 0);
    }

    void Input()         override {}
    void Update(float)   override {}
};

// ============================================================
// DashTrailRenderer (GDI 완전 제거 → PrimitiveBatch 사용)
// ============================================================
struct DashAfterimage
{
    float x, y;
    float alpha;
    float lifetime;
};

struct DashParticle
{
    float x, y;
    float vx, vy;
    float alpha;
    float lifetime;
    float size;
};

class DashTrailRenderer : public Component
{
    std::vector<DashAfterimage> afterimages;
    std::vector<DashParticle>   particles;

    std::mt19937                          rng;
    std::uniform_real_distribution<float> randAngle;
    std::uniform_real_distribution<float> randSpeed;
    std::uniform_real_distribution<float> randSize;

    float dashDirX = 0.0f, dashDirY = 0.0f;
    float dashStartX = 0.0f, dashStartY = 0.0f;
    float dashEndX = 0.0f, dashEndY = 0.0f;

    bool  spawningAfterimages = false;
    float afterimageTimer = 0.0f;
    int   afterimagesLeft = 0;

    static const int   AFTERIMAGE_COUNT = 5;
    static const float AFTERIMAGE_INTERVAL; // 정의는 아래
    static const float AFTERIMAGE_LIFE;
    static const int   PARTICLE_COUNT = 18;
    static const float PARTICLE_LIFE;

public:
    DashTrailRenderer()
        : rng(std::random_device{}())
        , randAngle(0.0f, 6.28318f)
        , randSpeed(60.0f, 200.0f)
        , randSize(3.0f, 7.0f)
    {}

    void Start(GraphicsContext*) override {}
    void Input()  override {}

    void OnDash(float startX, float startY, float endX, float endY,
        float dirX, float dirY)
    {
        dashDirX = dirX; dashDirY = dirY;
        dashStartX = startX; dashStartY = startY;
        dashEndX = endX;   dashEndY = endY;

        spawningAfterimages = true;
        afterimageTimer = 0.0f;
        afterimagesLeft = AFTERIMAGE_COUNT;

        for (int i = 0; i < PARTICLE_COUNT; ++i)
        {
            float angle = randAngle(rng);
            float speed = randSpeed(rng);
            float vx = cosf(angle) * speed * 0.4f + (-dirX) * speed * 0.6f;
            float vy = sinf(angle) * speed * 0.4f + (-dirY) * speed * 0.6f;

            DashParticle p;
            p.x = startX; p.y = startY;
            p.vx = vx;     p.vy = vy;
            p.alpha = 1.0f;
            p.lifetime = PARTICLE_LIFE;
            p.size = randSize(rng);
            particles.push_back(p);
        }
    }

    void Update(float dt) override
    {
        if (spawningAfterimages && afterimagesLeft > 0)
        {
            afterimageTimer += dt;
            while (afterimageTimer >= AFTERIMAGE_INTERVAL && afterimagesLeft > 0)
            {
                afterimageTimer -= AFTERIMAGE_INTERVAL;
                afterimagesLeft--;

                float t = 1.0f - (float)afterimagesLeft / AFTERIMAGE_COUNT;
                DashAfterimage img;
                img.x = dashStartX + (dashEndX - dashStartX) * t;
                img.y = dashStartY + (dashEndY - dashStartY) * t;
                img.alpha = 0.75f;
                img.lifetime = AFTERIMAGE_LIFE;
                afterimages.push_back(img);
            }
            if (afterimagesLeft <= 0) spawningAfterimages = false;
        }

        for (auto& img : afterimages)
        {
            img.lifetime -= dt;
            img.alpha = img.lifetime / AFTERIMAGE_LIFE * 0.75f;
        }
        afterimages.erase(
            std::remove_if(afterimages.begin(), afterimages.end(),
                [](const DashAfterimage& a) { return a.lifetime <= 0.0f; }),
            afterimages.end());

        for (auto& p : particles)
        {
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.lifetime -= dt;
            p.alpha = p.lifetime / PARTICLE_LIFE;
            p.vx *= (1.0f - dt * 3.0f);
            p.vy *= (1.0f - dt * 3.0f);
        }
        particles.erase(
            std::remove_if(particles.begin(), particles.end(),
                [](const DashParticle& p) { return p.lifetime <= 0.0f; }),
            particles.end());
    }

    // ── DirectX 렌더링 ────────────────────────────────────────
    void Render(GraphicsContext* gfx) override
    {
        if (gState != GameState::PLAYING) return;

        // 잔상: 함선 실루엣 삼각형 3개 (몸체 + 날개 2개)
        for (auto& img : afterimages)
        {
            float a = img.alpha;
            float hw = 12.0f, hh = 12.0f;
            XMFLOAT4 col = { 0.3f * a, 0.8f * a, a, a };

            // 몸체
            gfx->Batch.AddTri(
                { img.x,          img.y - hh * 2.0f },
                { img.x + hw,     img.y + hh },
                { img.x - hw,     img.y + hh }, col);
            // 왼쪽 날개
            gfx->Batch.AddTri(
                { img.x,              img.y + hh * 0.5f },
                { img.x - hw,         img.y + hh },
                { img.x - hw * 2.5f,  img.y + hh * 1.5f }, col);
            // 오른쪽 날개
            gfx->Batch.AddTri(
                { img.x,              img.y + hh * 0.5f },
                { img.x + hw * 2.5f,  img.y + hh * 1.5f },
                { img.x + hw,         img.y + hh }, col);
        }

        // 파티클: 작은 사각형
        for (auto& p : particles)
        {
            float a = p.alpha;
            XMFLOAT4 col = { a, a, a, a };
            gfx->Batch.AddRect(p.x - p.size, p.y - p.size,
                p.x + p.size, p.y + p.size, col);
        }
    }
};
const float DashTrailRenderer::AFTERIMAGE_INTERVAL = 0.018f;
const float DashTrailRenderer::AFTERIMAGE_LIFE = 0.25f;
const float DashTrailRenderer::PARTICLE_LIFE = 0.4f;

// ============================================================
// PlayerController (기존 유지, pTrail 타입만 변경)
// ============================================================
class PlayerController : public Component
{
    XMFLOAT2 moveDir = { 0, 0 };
    XMFLOAT2 lastDir = { 1, 0 };

public:
    DashTrailRenderer* pTrail = nullptr;

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

        if (gDashCooldown > 0.0f) gDashCooldown -= dt;

        float len = sqrtf(moveDir.x * moveDir.x + moveDir.y * moveDir.y);
        if (len > 0.0f) { lastDir.x = moveDir.x / len; lastDir.y = moveDir.y / len; }

        pOwner->posX += moveDir.x * PLAYER_SPEED * dt;
        pOwner->posY += moveDir.y * PLAYER_SPEED * dt;

        if (pOwner->posX < PLAYER_HALF_W)            pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W) pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)            pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H) pOwner->posY = SCREEN_H - PLAYER_HALF_H;

        if ((GetAsyncKeyState(VK_SPACE) & 0x0001) && gDashCooldown <= 0.0f)
            TryDash();
    }

    void TryDash()
    {
        float startX = pOwner->posX;
        float startY = pOwner->posY;

        pOwner->posX += lastDir.x * DASH_DISTANCE;
        pOwner->posY += lastDir.y * DASH_DISTANCE;

        if (pOwner->posX < PLAYER_HALF_W)            pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W) pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)            pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H) pOwner->posY = SCREEN_H - PLAYER_HALF_H;

        gDashCooldown = DASH_COOLDOWN;

        if (pTrail)
            pTrail->OnDash(startX, startY,
                pOwner->posX, pOwner->posY,
                lastDir.x, lastDir.y);

        printf("[Dash] 발동! 쿨타임 %.1f초 시작\n", DASH_COOLDOWN);
    }

    void Render(GraphicsContext*) override {}
};

// ============================================================
// BlinkRenderer (기존 유지)
// ============================================================
class BlinkRenderer : public Component
{
    ColorMaterial* pMat = nullptr;
    float          blinkTimer = 0.0f;
    bool           visible = true;
    std::mt19937   rng;
    std::uniform_real_distribution<float> randCol;

    static const float BLINK_INTERVAL;

public:
    BlinkRenderer(ColorMaterial* mat)
        : pMat(mat)
        , rng(std::random_device{}())
        , randCol(0.3f, 1.0f)
    {}

    void Start(GraphicsContext*) override {}
    void Input()                 override {}

    void Update(float dt) override
    {
        if (!pMat) return;

        if (gIsInvincible)
        {
            blinkTimer += dt;
            if (blinkTimer >= BLINK_INTERVAL)
            {
                visible = !visible;
                blinkTimer = 0.0f;

                if (visible)
                    pMat->SetColor({ randCol(rng), randCol(rng), randCol(rng), 1.0f });
                else
                    pMat->SetColor({ 0.0f, 0.0f, 0.0f, 0.0f });
            }
        }
        else
        {
            visible = true;
            blinkTimer = 0.0f;
            pMat->SetColor({ 0.3f, 0.8f, 1.0f, 1.0f });
        }
    }

    void Render(GraphicsContext*) override {}
};
const float BlinkRenderer::BLINK_INTERVAL = 0.1f;

// ============================================================
// ObstacleController (기존 유지)
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
// CooldownBarRenderer (GDI 완전 제거 → PrimitiveBatch 사용)
// 텍스트 레이블은 TextRenderer 에 위임하므로 여기서는 제거
// ============================================================
class CooldownBarRenderer : public Component
{
public:
    void Start(GraphicsContext*) override {}
    void Input()  override {}
    void Update(float) override {}

    void Render(GraphicsContext* gfx) override
    {
        if (gState != GameState::PLAYING) return;

        // 배경 (어두운 회색)
        gfx->Batch.AddRect(
            (float)(GAUGE_X - 2), (float)(GAUGE_Y - 2),
            (float)(GAUGE_X + GAUGE_W + 2), (float)(GAUGE_Y + GAUGE_H + 2),
            { 0.2f, 0.2f, 0.2f, 1.0f });

        // 채워진 비율
        float ratio = (gDashCooldown <= 0.0f) ? 1.0f
            : 1.0f - (gDashCooldown / DASH_COOLDOWN);
        int fillW = (int)(GAUGE_W * ratio);

        XMFLOAT4 gaugeCol = (gDashCooldown <= 0.0f)
            ? XMFLOAT4{ 0.39f, 0.86f, 1.0f, 1.0f }   // 완충: 하늘색
        : XMFLOAT4{ 0.16f, 0.39f, 0.71f, 1.0f };  // 충전 중: 어두운 청색

        if (fillW > 0)
            gfx->Batch.AddRect(
                (float)GAUGE_X, (float)GAUGE_Y,
                (float)(GAUGE_X + fillW), (float)(GAUGE_Y + GAUGE_H),
                gaugeCol);

        // 외곽선 (LINELIST)
        XMFLOAT4 outlineCol = { 0.7f, 0.7f, 0.7f, 1.0f };
        float x0 = (float)GAUGE_X, y0 = (float)GAUGE_Y;
        float x1 = (float)(GAUGE_X + GAUGE_W), y1 = (float)(GAUGE_Y + GAUGE_H);
        gfx->Batch.AddLine({ x0, y0 }, { x1, y0 }, outlineCol);
        gfx->Batch.AddLine({ x1, y0 }, { x1, y1 }, outlineCol);
        gfx->Batch.AddLine({ x1, y1 }, { x0, y1 }, outlineCol);
        gfx->Batch.AddLine({ x0, y1 }, { x0, y0 }, outlineCol);
    }
};

// ============================================================
// ShieldRenderer (GDI 완전 제거 → PrimitiveBatch LINELIST)
// ============================================================
class ShieldRenderer : public Component
{
    float rotAngle = 0.0f;
    float pulseTimer = 0.0f;

    enum { SIDES = 6 };
    static const float SHIELD_RADIUS;
    static const float ROTATE_SPEED;
    static const float PULSE_SPEED;

public:
    void Start(GraphicsContext*) override {}
    void Input()  override {}

    void Update(float dt) override
    {
        if (!gIsInvincible) return;
        rotAngle += ROTATE_SPEED * dt;
        pulseTimer += PULSE_SPEED * dt;
    }

    void Render(GraphicsContext* gfx) override
    {
        if (!gIsInvincible || gState != GameState::PLAYING) return;

        float ratio = gInvTimer / INVINCIBLE_DURATION;
        float pulse = sinf(pulseTimer) * 6.0f;
        float r = SHIELD_RADIUS + pulse;

        // 색상: 시간 남을수록 더 붉게
        float R = 1.0f;
        float G = 0.2f + 0.78f * ratio;
        float B = 0.2f * ratio;

        static const float TWO_PI = 6.28318530718f;

        for (int layer = 0; layer < 2; ++layer)
        {
            float lr = r + layer * 4.0f;
            float dim = (layer == 0) ? 1.0f : 0.5f;
            XMFLOAT4 col = { R * dim, G * dim, B * dim, 1.0f };

            std::vector<XMFLOAT2> pts(SIDES);
            for (int i = 0; i < SIDES; ++i)
            {
                float angle = rotAngle + (i * TWO_PI / SIDES);
                pts[i] = { pOwner->posX + cosf(angle) * lr,
                           pOwner->posY + sinf(angle) * lr };
            }
            gfx->Batch.AddPolyline(pts, col, true);
        }

        // 꼭짓점 강조 점 (작은 사각형)
        for (int i = 0; i < SIDES; ++i)
        {
            float angle = rotAngle + (i * TWO_PI / SIDES);
            float cx = pOwner->posX + cosf(angle) * r;
            float cy = pOwner->posY + sinf(angle) * r;
            gfx->Batch.AddRect(cx - 3, cy - 3, cx + 3, cy + 3,
                { 1.0f, 1.0f, 0.7f, 1.0f });
        }
    }
};
const float ShieldRenderer::SHIELD_RADIUS = 45.0f;
const float ShieldRenderer::ROTATE_SPEED = 1.8f;
const float ShieldRenderer::PULSE_SPEED = 4.0f;

// ============================================================
// TextRenderer
// 별 배경 → DirectX / 텍스트 → GDI 유지
// ============================================================
class TextRenderer
{
    HWND  hWnd = nullptr;
    HFONT hFont = nullptr;
    HFONT hFontLarge = nullptr;

    struct Star { float x, y; float brightness; };
    std::vector<Star> stars;

    GraphicsContext* gfx = nullptr;

public:
    void Initialize(HWND hwnd, GraphicsContext* gfxPtr)
    {
        hWnd = hwnd;
        gfx = gfxPtr;

        hFont = CreateFont(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        hFontLarge = CreateFont(56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");

        // 별 200개 생성 (1픽셀 점 대신 2×2 사각형으로 표현)
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> randX(0.0f, (float)SCREEN_W);
        std::uniform_real_distribution<float> randY(0.0f, (float)SCREEN_H);
        std::uniform_real_distribution<float> randB(0.4f, 1.0f);
        for (int i = 0; i < 200; ++i)
            stars.push_back({ randX(rng), randY(rng), randB(rng) });
    }

    // 별 배경만 DirectX 로 렌더링 (Render() 앞부분에서 호출)
    void RenderStars()
    {
        for (auto& s : stars)
        {
            XMFLOAT4 col = { s.brightness, s.brightness, s.brightness, 1.0f };
            gfx->Batch.AddRect(s.x - 1, s.y - 1, s.x + 1, s.y + 1, col);
        }
    }

    void DrawText(const std::wstring& text, int x, int y,
        COLORREF color, bool large = false)
    {
        HDC hdc = GetDC(hWnd);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        SelectObject(hdc, large ? hFontLarge : hFont);
        ::TextOut(hdc, x, y, text.c_str(), (int)text.length());
        ReleaseDC(hWnd, hdc);
    }

    // GDI 텍스트 렌더링 (Present 이후 호출)
    void RenderText()
    {
        if (gState == GameState::MAIN)
        {
            DrawText(L"DODGE++", 430, 200, RGB(255, 255, 255), true);
            DrawText(L"SPACE  :  게임 시작", 460, 370, RGB(200, 200, 200));
            DrawText(L"방향키 :  이동", 460, 410, RGB(200, 200, 200));
            DrawText(L"SPACE  :  대시 (쿨 3초)", 460, 450, RGB(100, 220, 255));
            DrawText(L"★ 노란 별 = 무적 아이템", 460, 490, RGB(255, 230, 50));
            DrawText(L"ESC    :  종료", 460, 530, RGB(200, 200, 200));

            std::wstring hiText = L"BEST  : " + std::to_wstring((int)gHighScore);
            DrawText(hiText, 510, 320, RGB(100, 255, 180));
        }
        else if (gState == GameState::PLAYING)
        {
            std::wstring scoreText = L"SCORE : " + std::to_wstring((int)gScore);
            DrawText(scoreText, 10, 10, RGB(255, 255, 100));

            if (gIsInvincible)
            {
                std::wstring invText = L"★ 무적  " +
                    std::to_wstring((int)(gInvTimer + 1.0f)) + L"s";
                DrawText(invText, SCREEN_W / 2 - 80, 10, RGB(255, 220, 50));
            }

            // 쿨타임 레이블 (CooldownBarRenderer 에서 GDI 제거했으므로 여기서 처리)
            std::wstring label = (gDashCooldown <= 0.0f)
                ? L"DASH  READY"
                : (L"DASH  " + std::to_wstring((int)(gDashCooldown + 1.0f)) + L"s");
            DrawText(label, GAUGE_X, GAUGE_Y - 22, RGB(220, 220, 220));
        }
        else if (gState == GameState::GAMEOVER)
        {
            DrawText(L"GAME OVER", 440, 240, RGB(255, 80, 80), true);
            std::wstring scoreText = L"SCORE : " + std::to_wstring((int)gScore);
            DrawText(scoreText, 510, 340, RGB(255, 255, 100));
            std::wstring hiText = L"BEST  : " + std::to_wstring((int)gHighScore);
            DrawText(hiText, 510, 380, RGB(100, 255, 180));
            DrawText(L"R  :  재시작", 510, 430, RGB(200, 200, 200));
            DrawText(L"ESC  :  종료", 510, 470, RGB(200, 200, 200));
            DrawText(L"M  :  메인으로", 510, 510, RGB(180, 220, 255));
        }
    }

    ~TextRenderer()
    {
        if (hFont)      DeleteObject(hFont);
        if (hFontLarge) DeleteObject(hFontLarge);
    }
};

// ============================================================
// GameManager (기존 유지)
// ============================================================
class GameManager
{
    std::mt19937                          rng;
    std::uniform_real_distribution<float> randPosX;
    std::uniform_real_distribution<float> randPosY;
    std::uniform_real_distribution<float> randSpeed;
    std::uniform_int_distribution<int>    randSide;
    std::uniform_real_distribution<float> randDiag;

    float spawnTimer = 0.0f;
    float itemSpawnTimer = 0.0f;
    int   difficultyLevel = 0;
    float difficultyTimer = 0.0f;

public:
    std::vector<GameObject*>* obstacles = nullptr;
    std::vector<GameObject*>* items = nullptr;
    GameObject* player = nullptr;
    GraphicsContext* gfx = nullptr;

    Mesh* obsMesh = nullptr;
    Material* obsMat = nullptr;
    Mesh* itemMesh = nullptr;
    Material* itemMat = nullptr;

    GameManager()
        : rng(std::random_device{}())
        , randPosX(ITEM_HALF_W + 50, SCREEN_W - ITEM_HALF_W - 50)
        , randPosY(ITEM_HALF_H + 50, SCREEN_H - ITEM_HALF_H - 50)
        , randSpeed(OBS_SPEED_MIN, OBS_SPEED_MAX)
        , randSide(0, 3)
        , randDiag(-1.0f, 1.0f)
    {}

    void CalcObsSpawn(float& outX, float& outY, float& outVX, float& outVY)
    {
        float sMin, sMax;
        GetSpeedRange(sMin, sMax);
        std::uniform_real_distribution<float> dynSpeed(sMin, sMax);

        int   side = randSide(rng);
        float speed = dynSpeed(rng);
        float diag = randDiag(rng);
        switch (side)
        {
        case 0: outX = randPosX(rng); outY = -OBS_HALF_H;
            outVX = speed * diag; outVY = speed;  break;
        case 1: outX = randPosX(rng); outY = SCREEN_H + OBS_HALF_H;
            outVX = speed * diag; outVY = -speed; break;
        case 2: outX = -OBS_HALF_W;  outY = randPosY(rng);
            outVX = speed; outVY = speed * diag;  break;
        default:outX = SCREEN_W + OBS_HALF_W; outY = randPosY(rng);
            outVX = -speed; outVY = speed * diag; break;
        }
    }

    void ResetGame()
    {
        gScore = 0.0f;
        spawnTimer = itemSpawnTimer = 0.0f;
        gIsInvincible = false;
        gInvTimer = gDashCooldown = 0.0f;
        difficultyLevel = 0;
        difficultyTimer = 0.0f;

        GetAsyncKeyState(VK_SPACE);

        if (player) { player->posX = SCREEN_W / 2.0f; player->posY = SCREEN_H / 2.0f; player->active = true; }
        if (obstacles) for (auto* o : *obstacles) o->active = false;
        if (items)     for (auto* i : *items)     i->active = false;

        printf("[Game] Reset. PLAYING 시작!\n");
    }

    void SpawnObstacles(int count)
    {
        for (int n = 0; n < count; ++n)
        {
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

    void SpawnInvincibleItem()
    {
        if (!items || !gfx || !itemMesh || !itemMat) return;
        for (auto* it : *items) if (it->active) return;

        float x = randPosX(rng);
        float y = randPosY(rng);

        for (auto* it : *items)
        {
            if (!it->active) { it->posX = x; it->posY = y; it->active = true; return; }
        }

        auto* item = new GameObject(x, y, ITEM_HALF_W, ITEM_HALF_H);
        item->AddComponent(new MeshRenderer(itemMesh, itemMat));
        items->push_back(item);
        printf("[Item] 무적 아이템 스폰 (%.0f, %.0f)\n", x, y);
    }

    void UpdateInvincible(float dt)
    {
        if (!gIsInvincible) return;
        gInvTimer -= dt;
        if (gInvTimer <= 0.0f) { gIsInvincible = false; gInvTimer = 0.0f; printf("[Invincible] 무적 종료\n"); }
    }

    float GetSpawnInterval() const
    {
        float t = (float)difficultyLevel / DIFFICULTY_MAX;
        return OBS_SPAWN_INTERVAL * (1.0f - t * 0.67f);
    }

    void GetSpeedRange(float& outMin, float& outMax) const
    {
        outMin = OBS_SPEED_MIN + difficultyLevel * 40.0f;
        outMax = OBS_SPEED_MAX + difficultyLevel * 60.0f;
    }

    int GetSpawnCount() const { return 4 + difficultyLevel; }

    void Update(float dt)
    {
        if (gState != GameState::PLAYING) return;

        gScore += dt;

        difficultyTimer += dt;
        if (difficultyTimer >= DIFFICULTY_INTERVAL && difficultyLevel < DIFFICULTY_MAX)
        {
            difficultyLevel++;
            difficultyTimer = 0.0f;
            printf("[Difficulty] 단계 %d 돌입!\n", difficultyLevel);
        }

        spawnTimer += dt;
        if (spawnTimer >= GetSpawnInterval()) { SpawnObstacles(GetSpawnCount()); spawnTimer = 0.0f; }

        itemSpawnTimer += dt;
        if (itemSpawnTimer >= ITEM_SPAWN_INTERVAL) { SpawnInvincibleItem(); itemSpawnTimer = 0.0f; }

        UpdateInvincible(dt);

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
    std::vector<GameObject*> items;
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
        text.Initialize(win.hWnd, &gfx);
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

        if (gState == GameState::GAMEOVER && (GetAsyncKeyState('M') & 0x0001))
        {
            gState = GameState::MAIN; manager.ResetGame();
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
        // ── 1. RTV Clear ─────────────────────────────────────
        float col[] = { 0.0f, 0.0f, 0.05f, 1.0f };
        gfx.ImmediateContext->ClearRenderTargetView(gfx.RTV, col);

        D3D11_VIEWPORT vp = { 0, 0, (float)SCREEN_W, (float)SCREEN_H, 0, 1 };
        gfx.ImmediateContext->RSSetViewports(1, &vp);
        gfx.ImmediateContext->OMSetRenderTargets(1, &gfx.RTV, NULL);
        gfx.ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 블렌드 스테이트
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
        float blendFactor[4] = {};
        gfx.ImmediateContext->OMSetBlendState(blendState, blendFactor, 0xffffffff);
        blendState->Release();

        // ── 2. 별 배경 (DirectX) ─────────────────────────────
        text.RenderStars();

        // ── 3. 정적 Mesh 오브젝트 렌더링 ────────────────────
        for (auto* obs : obstacles) if (obs && obs->active) obs->Render(&gfx);
        for (auto* it : items)     if (it && it->active)  it->Render(&gfx);
        for (auto* obj : world)     if (obj && obj->active)  obj->Render(&gfx);

        // ── 4. PrimitiveBatch 일괄 제출 ──────────────────────
        //    (DashTrailRenderer, CooldownBarRenderer, ShieldRenderer 가
        //     Render() 안에서 Batch.AddXxx 를 호출했으므로 여기서 한 번에 GPU 업로드)
        gfx.Batch.Flush(gfx.ImmediateContext);

        // ── 5. Present → GDI 텍스트 오버레이 ─────────────────
        gfx.SwapChain->Present(gfx.VSync, 0);
        text.RenderText();
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
// 셰이더 소스
// b0: World 행렬  b1: tintColor
// PrimitiveBatch 는 tintColor = {1,1,1,1} 고정 → 버텍스 col 사용
// ============================================================
static const std::string SHADER_SRC = R"(
    cbuffer cbWorld    : register(b0) { matrix matWorld;  }
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

    // tintColor 가 흰색(1,1,1,1)이면 버텍스 색 그대로 출력
    // ColorMaterial 이 tintColor 를 설정하면 단색 오버라이드
    float4 PS(PS_IN input) : SV_Target
    {
        return input.col * tintColor;
    }
)";

// ============================================================
// Mesh 생성 헬퍼
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

Mesh* CreateStarMesh(ID3D11Device* device, float outerR, float innerR)
{
    float nOutX = (outerR / SCREEN_W) * 2.0f;
    float nOutY = (outerR / SCREEN_H) * 2.0f;
    float nInX = (innerR / SCREEN_W) * 2.0f;
    float nInY = (innerR / SCREEN_H) * 2.0f;

    XMFLOAT2 pts[10];
    for (int i = 0; i < 10; ++i)
    {
        float angle = XM_PIDIV2 - (i * XM_2PI / 10.0f);
        if (i % 2 == 0) pts[i] = { cosf(angle) * nOutX, sinf(angle) * nOutY };
        else            pts[i] = { cosf(angle) * nInX,  sinf(angle) * nInY };
    }

    std::vector<Vertex> verts;
    verts.reserve(30);
    for (int i = 0; i < 10; ++i)
    {
        verts.push_back({ { 0, 0, 0 },                                          { 1,1,1,1 } });
        verts.push_back({ { pts[i].x, pts[i].y, 0 },                            { 1,1,1,1 } });
        verts.push_back({ { pts[(i + 1) % 10].x, pts[(i + 1) % 10].y, 0 },              { 1,1,1,1 } });
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
    verts.push_back({ {  0.0f,      nH * 2.0f,  0 }, { 1,1,1,1 } });
    verts.push_back({ {  nW,       -nH,          0 }, { 1,1,1,1 } });
    verts.push_back({ { -nW,       -nH,          0 }, { 1,1,1,1 } });
    verts.push_back({ {  0.0f,     -nH * 0.5f,  0 }, { 1,1,1,1 } });
    verts.push_back({ { -nW,       -nH,          0 }, { 1,1,1,1 } });
    verts.push_back({ { -nW * 2.5f,-nH * 1.5f,  0 }, { 1,1,1,1 } });
    verts.push_back({ {  0.0f,     -nH * 0.5f,  0 }, { 1,1,1,1 } });
    verts.push_back({ {  nW * 2.5f,-nH * 1.5f,  0 }, { 1,1,1,1 } });
    verts.push_back({ {  nW,       -nH,          0 }, { 1,1,1,1 } });
    Mesh* mesh = new Mesh();
    mesh->Create(device, verts);
    return mesh;
}

Mesh* CreateMeteorMesh(ID3D11Device* device, float r)
{
    const int POINTS = 8;
    float nW = (r / SCREEN_W) * 2.0f;
    float nH = (r / SCREEN_H) * 2.0f;
    XMFLOAT2 pts[POINTS];
    for (int i = 0; i < POINTS; ++i)
    {
        float angle = (i * XM_2PI / POINTS);
        pts[i] = { cosf(angle) * nW, sinf(angle) * nH };
    }
    std::vector<Vertex> verts;
    verts.reserve(POINTS * 3);
    for (int i = 0; i < POINTS; ++i)
    {
        verts.push_back({ { 0, 0, 0 },                                              { 1,1,1,1 } });
        verts.push_back({ { pts[(i + 1) % POINTS].x, pts[(i + 1) % POINTS].y, 0 },          { 1,1,1,1 } });
        verts.push_back({ { pts[i].x, pts[i].y, 0 },                                { 1,1,1,1 } });
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

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    ShaderSet shaders = gEngine.gfx.CompileAndCreate(SHADER_SRC, ied, 2);
    if (!shaders.vs || !shaders.ps)
    {
        printf("[Error] Shader compile failed.\n"); return -1;
    }

    // PrimitiveBatch 초기화 (셰이더 공유)
    if (!gEngine.gfx.Batch.Init(gEngine.gfx.Device, shaders))
    {
        printf("[Error] PrimitiveBatch init failed.\n"); return -1;
    }

    Mesh* playerMesh = CreateShipMesh(gEngine.gfx.Device, 15.0f);
    Mesh* obsMesh = CreateMeteorMesh(gEngine.gfx.Device, 15.0f);
    Mesh* itemMesh = CreateStarMesh(gEngine.gfx.Device, ITEM_HALF_W, ITEM_HALF_W * 0.42f);

    ColorMaterial* playerMat = new ColorMaterial(shaders, { 0.3f, 0.8f, 1.0f, 1.0f }, gEngine.gfx.Device);
    ColorMaterial* obsMat = new ColorMaterial(shaders, { 1.0f, 0.5f, 0.2f, 1.0f }, gEngine.gfx.Device);
    ColorMaterial* itemMat = new ColorMaterial(shaders, { 1.0f, 0.9f, 0.1f, 1.0f }, gEngine.gfx.Device);

    // 플레이어 조립
    auto* player = new GameObject(SCREEN_W / 2.0f, SCREEN_H / 2.0f, PLAYER_HALF_W, PLAYER_HALF_H);
    auto* trail = new DashTrailRenderer();

    player->AddComponent(new MeshRenderer(playerMesh, playerMat));  // [0] 셰이프
    player->AddComponent(new PlayerController());                    // [1] 입력/이동
    player->AddComponent(new BlinkRenderer(playerMat));              // [2] 무적 깜빡임
    player->AddComponent(new CooldownBarRenderer());                 // [3] 게이지 UI
    player->AddComponent(new ShieldRenderer());                      // [4] 쉴드
    player->AddComponent(trail);                                     // [5] 대시 잔상

    auto* pc = dynamic_cast<PlayerController*>(player->components[1]);
    if (pc) pc->pTrail = trail;

    gEngine.world.push_back(player);

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

    delete playerMat;
    delete obsMat;
    delete itemMat;
    delete playerMesh;
    delete obsMesh;
    delete itemMesh;
    shaders.Release();

    return 0;
}
