/*
================================================================================
 Dodge++
--------------------------------------------------------------------------------
 렌더링 순서
  1. ClearRenderTargetView
  2. gUIObj::Render      → StarBackgroundRenderer → Batch 누적
  3. MeshRenderer::Render (장애물·아이템·플레이어)
  4. CooldownBar/Shield/DashTrail::Render → Batch 누적
  5. Batch.Flush         → TRIANGLELIST / LINELIST 일괄 GPU 제출
  6. gUITextObj::Render  → UITextRenderer → 비트맵 폰트 쿼드 Draw
  7. SwapChain::Present

 [조원]
 조장 - 김정일 (12211586) - A (로직 담당)
 조원 - 안시헌 (12211645) - B (렌더링 담당)
================================================================================
*/

#pragma once
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
struct Vertex { XMFLOAT3 pos; XMFLOAT4 col; };
struct ConstantBuffer { XMMATRIX matWorld; };
struct ColorBuffer { XMFLOAT4 tintColor; };

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
// Mesh
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
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = sizeof(Vertex) * vertexCount;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = vertices.data();
        device->CreateBuffer(&bd, &sd, &vBuffer);
    }
};

// ============================================================
// PrimitiveBatch
// ============================================================
class PrimitiveBatch
{
    static const UINT MAX_VERTS = 65536;

    ID3D11Buffer* dynBuf = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* cbWorld = nullptr;
    ID3D11Buffer* cbColor = nullptr;

    std::vector<Vertex> triVerts;
    std::vector<Vertex> lineVerts;

public:
    bool Init(ID3D11Device* device, ShaderSet shaders)
    {
        vs = shaders.vs; ps = shaders.ps; layout = shaders.layout;

        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(Vertex) * MAX_VERTS;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &dynBuf))) return false;

        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(ConstantBuffer);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ConstantBuffer identity;
        identity.matWorld = XMMatrixTranspose(XMMatrixIdentity());
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = &identity;
        device->CreateBuffer(&cbd, &sd, &cbWorld);

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
    }

    void AddTri(XMFLOAT2 a, XMFLOAT2 b, XMFLOAT2 c, XMFLOAT4 col)
    {
        if (triVerts.size() + 3 > MAX_VERTS) return;
        triVerts.push_back({ { PxToNdcX(a.x), PxToNdcY(a.y), 0 }, col });
        triVerts.push_back({ { PxToNdcX(b.x), PxToNdcY(b.y), 0 }, col });
        triVerts.push_back({ { PxToNdcX(c.x), PxToNdcY(c.y), 0 }, col });
    }

    void AddRect(float x0, float y0, float x1, float y1, XMFLOAT4 col)
    {
        AddTri({ x0,y0 }, { x1,y0 }, { x1,y1 }, col);
        AddTri({ x0,y0 }, { x1,y1 }, { x0,y1 }, col);
    }

    void AddLine(XMFLOAT2 a, XMFLOAT2 b, XMFLOAT4 col)
    {
        if (lineVerts.size() + 2 > MAX_VERTS) return;
        lineVerts.push_back({ { PxToNdcX(a.x), PxToNdcY(a.y), 0 }, col });
        lineVerts.push_back({ { PxToNdcX(b.x), PxToNdcY(b.y), 0 }, col });
    }

    void AddPolyline(const std::vector<XMFLOAT2>& pts, XMFLOAT4 col, bool close = true)
    {
        int n = (int)pts.size();
        for (int i = 0; i < n - 1; ++i) AddLine(pts[i], pts[i + 1], col);
        if (close && n > 1) AddLine(pts[n - 1], pts[0], col);
    }

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

        if (!triVerts.empty())
        {
            Upload(ctx, triVerts);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->Draw((UINT)triVerts.size(), 0);
            triVerts.clear();
        }
        if (!lineVerts.empty())
        {
            Upload(ctx, lineVerts);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            ctx->Draw((UINT)lineVerts.size(), 0);
            lineVerts.clear();
        }
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
// Material / ColorMaterial
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

    void Bind(ID3D11DeviceContext* ctx) override
    {
        ctx->IASetInputLayout(shaders.layout);
        ctx->VSSetShader(shaders.vs, nullptr, 0);
        ctx->PSSetShader(shaders.ps, nullptr, 0);
        ColorBuffer cb = { color };
        ctx->UpdateSubresource(pColorBuffer, 0, nullptr, &cb, 0, 0);
        ctx->PSSetConstantBuffers(1, 1, &pColorBuffer);
    }
};

// ============================================================
// DeltaTime / WindowContext
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
        hWnd = CreateWindow(L"DX11Engine", L"Dodge++",
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
    PrimitiveBatch          Batch;

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
            NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &sd, &SwapChain, &Device, NULL, &ImmediateContext);
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
        ID3DBlob* vsBlob = nullptr, * psBlob = nullptr, * errBlob = nullptr;

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
// Component (순수 추상 기저)
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

// ============================================================
// GameObject
// ============================================================
class GameObject
{
public:
    float posX = 0, posY = 0;
    float halfW = 0, halfH = 0;
    bool  active = true;
    std::vector<Component*> components;

    GameObject(float x = 0, float y = 0, float hw = 0, float hh = 0)
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
// MeshRenderer
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

    void Input()       override {}
    void Update(float) override {}
};

// ============================================================
// DashTrailRenderer
// ============================================================
struct DashAfterimage { float x, y, alpha, lifetime; };
struct DashParticle { float x, y, vx, vy, alpha, lifetime, size; };

class DashTrailRenderer : public Component
{
    std::vector<DashAfterimage> afterimages;
    std::vector<DashParticle>   particles;

    std::mt19937 rng;
    std::uniform_real_distribution<float> randAngle{ 0.0f, 6.28318f };
    std::uniform_real_distribution<float> randSpeed{ 60.0f, 200.0f };
    std::uniform_real_distribution<float> randSize{ 3.0f,  7.0f };

    float dashDirX = 0, dashDirY = 0;
    float dashStartX = 0, dashStartY = 0, dashEndX = 0, dashEndY = 0;
    bool  spawningAfterimages = false;
    float afterimageTimer = 0.0f;
    int   afterimagesLeft = 0;

    static const int   AFTERIMAGE_COUNT = 5;
    static const float AFTERIMAGE_INTERVAL;
    static const float AFTERIMAGE_LIFE;
    static const int   PARTICLE_COUNT = 18;
    static const float PARTICLE_LIFE;

public:
    DashTrailRenderer() : rng(std::random_device{}()) {}

    void Start(GraphicsContext*) override {}
    void Input()  override {}

    void OnDash(float sx, float sy, float ex, float ey, float dx, float dy)
    {
        dashDirX = dx; dashDirY = dy;
        dashStartX = sx; dashStartY = sy;
        dashEndX = ex; dashEndY = ey;
        spawningAfterimages = true;
        afterimageTimer = 0.0f;
        afterimagesLeft = AFTERIMAGE_COUNT;

        for (int i = 0; i < PARTICLE_COUNT; ++i)
        {
            float angle = randAngle(rng), speed = randSpeed(rng);
            DashParticle p;
            p.x = sx; p.y = sy;
            p.vx = cosf(angle) * speed * 0.4f + (-dx) * speed * 0.6f;
            p.vy = sinf(angle) * speed * 0.4f + (-dy) * speed * 0.6f;
            p.alpha = 1.0f; p.lifetime = PARTICLE_LIFE; p.size = randSize(rng);
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
                img.alpha = 0.75f; img.lifetime = AFTERIMAGE_LIFE;
                afterimages.push_back(img);
            }
            if (afterimagesLeft <= 0) spawningAfterimages = false;
        }

        for (auto& img : afterimages) { img.lifetime -= dt; img.alpha = img.lifetime / AFTERIMAGE_LIFE * 0.75f; }
        afterimages.erase(std::remove_if(afterimages.begin(), afterimages.end(),
            [](const DashAfterimage& a) { return a.lifetime <= 0.0f; }), afterimages.end());

        for (auto& p : particles)
        {
            p.x += p.vx * dt; p.y += p.vy * dt;
            p.lifetime -= dt; p.alpha = p.lifetime / PARTICLE_LIFE;
            p.vx *= (1.0f - dt * 3.0f); p.vy *= (1.0f - dt * 3.0f);
        }
        particles.erase(std::remove_if(particles.begin(), particles.end(),
            [](const DashParticle& p) { return p.lifetime <= 0.0f; }), particles.end());
    }

    void Render(GraphicsContext* gfx) override
    {
        if (gState != GameState::PLAYING) return;
        for (auto& img : afterimages)
        {
            float a = img.alpha, hw = 12.0f, hh = 12.0f;
            XMFLOAT4 col = { 0.3f * a, 0.8f * a, a, a };
            gfx->Batch.AddTri({ img.x,img.y - hh * 2 }, { img.x + hw,img.y + hh }, { img.x - hw,img.y + hh }, col);
            gfx->Batch.AddTri({ img.x,img.y + hh * .5f }, { img.x - hw,img.y + hh }, { img.x - hw * 2.5f,img.y + hh * 1.5f }, col);
            gfx->Batch.AddTri({ img.x,img.y + hh * .5f }, { img.x + hw * 2.5f,img.y + hh * 1.5f }, { img.x + hw,img.y + hh }, col);
        }
        for (auto& p : particles)
        {
            XMFLOAT4 col = { p.alpha,p.alpha,p.alpha,p.alpha };
            gfx->Batch.AddRect(p.x - p.size, p.y - p.size, p.x + p.size, p.y + p.size, col);
        }
    }
};
const float DashTrailRenderer::AFTERIMAGE_INTERVAL = 0.018f;
const float DashTrailRenderer::AFTERIMAGE_LIFE = 0.25f;
const float DashTrailRenderer::PARTICLE_LIFE = 0.4f;

// ============================================================
// PlayerController
// ============================================================
class PlayerController : public Component
{
    XMFLOAT2 moveDir = { 0,0 };
    XMFLOAT2 lastDir = { 1,0 };

public:
    DashTrailRenderer* pTrail = nullptr;

    void Start(GraphicsContext*) override
    {
        printf("[Player] Started.\n");
    }

    void Input() override
    {
        moveDir = { 0,0 };
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

        if (pOwner->posX < PLAYER_HALF_W)             pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W)  pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)             pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H)  pOwner->posY = SCREEN_H - PLAYER_HALF_H;

        if ((GetAsyncKeyState(VK_SPACE) & 0x0001) && gDashCooldown <= 0.0f)
            TryDash();
    }

    void TryDash()
    {
        float startX = pOwner->posX, startY = pOwner->posY;
        pOwner->posX += lastDir.x * DASH_DISTANCE;
        pOwner->posY += lastDir.y * DASH_DISTANCE;

        if (pOwner->posX < PLAYER_HALF_W)             pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W)  pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)             pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H)  pOwner->posY = SCREEN_H - PLAYER_HALF_H;

        gDashCooldown = DASH_COOLDOWN;
        if (pTrail)
            pTrail->OnDash(startX, startY, pOwner->posX, pOwner->posY, lastDir.x, lastDir.y);
        printf("[Dash] 발동! 쿨타임 %.1f초\n", DASH_COOLDOWN);
    }

    void Render(GraphicsContext*) override {}
};

// ============================================================
// BlinkRenderer
// ============================================================
class BlinkRenderer : public Component
{
    ColorMaterial* pMat = nullptr;
    float          blinkTimer = 0.0f;
    bool           visible = true;
    std::mt19937   rng;
    std::uniform_real_distribution<float> randCol{ 0.3f, 1.0f };

    static const float BLINK_INTERVAL;

public:
    BlinkRenderer(ColorMaterial* mat) : pMat(mat), rng(std::random_device{}()) {}

    void Start(GraphicsContext*) override {}
    void Input()  override {}

    void Update(float dt) override
    {
        if (!pMat) return;
        if (gIsInvincible)
        {
            blinkTimer += dt;
            if (blinkTimer >= BLINK_INTERVAL)
            {
                visible = !visible; blinkTimer = 0.0f;
                if (visible) pMat->SetColor({ randCol(rng), randCol(rng), randCol(rng), 1.0f });
                else         pMat->SetColor({ 0,0,0,0 });
            }
        }
        else
        {
            visible = true; blinkTimer = 0.0f;
            pMat->SetColor({ 0.3f, 0.8f, 1.0f, 1.0f });
        }
    }

    void Render(GraphicsContext*) override {}
};
const float BlinkRenderer::BLINK_INTERVAL = 0.1f;

// ============================================================
// ObstacleController
// ============================================================
class ObstacleController : public Component
{
public:
    float velX = 0.0f, velY = 0.0f;

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
// CooldownBarRenderer  (Component)
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

        gfx->Batch.AddRect((float)(GAUGE_X - 2), (float)(GAUGE_Y - 2),
            (float)(GAUGE_X + GAUGE_W + 2), (float)(GAUGE_Y + GAUGE_H + 2),
            { 0.2f,0.2f,0.2f,1.0f });

        float ratio = (gDashCooldown <= 0.0f) ? 1.0f : 1.0f - (gDashCooldown / DASH_COOLDOWN);
        int   fillW = (int)(GAUGE_W * ratio);

        XMFLOAT4 gaugeCol = (gDashCooldown <= 0.0f)
            ? XMFLOAT4{ 0.39f,0.86f,1.0f,1.0f }
        : XMFLOAT4{ 0.16f,0.39f,0.71f,1.0f };

        if (fillW > 0)
            gfx->Batch.AddRect((float)GAUGE_X, (float)GAUGE_Y,
                (float)(GAUGE_X + fillW), (float)(GAUGE_Y + GAUGE_H), gaugeCol);

        XMFLOAT4 outlineCol = { 0.7f,0.7f,0.7f,1.0f };
        float x0 = (float)GAUGE_X, y0 = (float)GAUGE_Y;
        float x1 = (float)(GAUGE_X + GAUGE_W), y1 = (float)(GAUGE_Y + GAUGE_H);
        gfx->Batch.AddLine({ x0,y0 }, { x1,y0 }, outlineCol);
        gfx->Batch.AddLine({ x1,y0 }, { x1,y1 }, outlineCol);
        gfx->Batch.AddLine({ x1,y1 }, { x0,y1 }, outlineCol);
        gfx->Batch.AddLine({ x0,y1 }, { x0,y0 }, outlineCol);
    }
};

// ============================================================
// ShieldRenderer  (Component)
// ============================================================
class ShieldRenderer : public Component
{
    float rotAngle = 0.0f, pulseTimer = 0.0f;
    enum { SIDES = 6 };
    static const float SHIELD_RADIUS, ROTATE_SPEED, PULSE_SPEED;

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
        float r = SHIELD_RADIUS + sinf(pulseTimer) * 6.0f;
        float R = 1.0f, G = 0.2f + 0.78f * ratio, B = 0.2f * ratio;

        static const float TWO_PI = 6.28318530718f;
        for (int layer = 0; layer < 2; ++layer)
        {
            float lr = r + layer * 4.0f, dim = (layer == 0) ? 1.0f : 0.5f;
            XMFLOAT4 col = { R * dim, G * dim, B * dim, 1.0f };
            std::vector<XMFLOAT2> pts(SIDES);
            for (int i = 0; i < SIDES; ++i)
            {
                float angle = rotAngle + (i * TWO_PI / SIDES);
                pts[i] = { pOwner->posX + cosf(angle) * lr, pOwner->posY + sinf(angle) * lr };
            }
            gfx->Batch.AddPolyline(pts, col, true);
        }
        for (int i = 0; i < SIDES; ++i)
        {
            float angle = rotAngle + (i * TWO_PI / SIDES);
            float cx = pOwner->posX + cosf(angle) * r;
            float cy = pOwner->posY + sinf(angle) * r;
            gfx->Batch.AddRect(cx - 3, cy - 3, cx + 3, cy + 3, { 1,1,0.7f,1 });
        }
    }
};
const float ShieldRenderer::SHIELD_RADIUS = 45.0f;
const float ShieldRenderer::ROTATE_SPEED = 1.8f;
const float ShieldRenderer::PULSE_SPEED = 4.0f;

// ============================================================
// 비트맵 폰트 상수
// ============================================================
static const int FONT_CELL_W = 32, FONT_CELL_H = 48;
static const int FONT_COLS = 16, FONT_ROWS = 3;
static const int FONT_TEX_W = FONT_CELL_W * FONT_COLS;
static const int FONT_TEX_H = FONT_CELL_H * FONT_ROWS;

// ============================================================
// StarBackgroundRenderer  (신규 Component)
// 역할  : 별 200개 PrimitiveBatch 렌더 + 매 프레임 스크롤
// 부착처: gUIObj
// ============================================================
class StarBackgroundRenderer : public Component
{
    struct Star { float x, y, brightness, vy; };
    std::vector<Star> stars;

public:
    void Start(GraphicsContext*) override
    {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> rX(0.f, (float)SCREEN_W);
        std::uniform_real_distribution<float> rY(0.f, (float)SCREEN_H);
        std::uniform_real_distribution<float> rB(0.4f, 1.0f);
        std::uniform_real_distribution<float> rV(20.0f, 60.0f);
        for (int i = 0; i < 200; ++i)
            stars.push_back({ rX(rng), rY(rng), rB(rng), rV(rng) });
        printf("[StarBG] Started. 별 %d개\n", (int)stars.size());
    }

    void Input() override {}

    void Update(float dt) override
    {
        for (auto& s : stars)
        {
            s.y += s.vy * dt;
            if (s.y > SCREEN_H + 2) s.y = -2.0f;
        }
    }

    void Render(GraphicsContext* gfx) override
    {
        for (auto& s : stars)
        {
            XMFLOAT4 col = { s.brightness,s.brightness,s.brightness,1.0f };
            gfx->Batch.AddRect(s.x - 1, s.y - 1, s.x + 1, s.y + 1, col);
        }
    }
};

// ============================================================
// UITextRenderer
// 역할  : 비트맵 폰트 텍스처 생성(1회) + 매 프레임 UI 텍스트 Draw
// 부착처: gUITextObj  (Batch.Flush 이후 Draw 순서 보장)
// Start(): GDI -> D3D11 텍스처 변환 후 GDI 즉시 해제
// ============================================================
class UITextRenderer : public Component
{
    ID3D11ShaderResourceView* fontSRV = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* identityCB = nullptr;
    ID3D11VertexShader* texVS = nullptr;
    ID3D11PixelShader* texPS = nullptr;
    ID3D11InputLayout* texLayout = nullptr;
    ID3D11Buffer* texVB = nullptr;

    static const UINT TEX_VB_MAX = 6 * 1024;

    struct TexVertex { XMFLOAT3 pos; XMFLOAT2 uv; XMFLOAT4 col; };
    std::vector<TexVertex> texVerts;

    static const std::string TEX_SHADER_SRC;

    HWND hWnd = nullptr;

public:
    void SetHWND(HWND hwnd) { hWnd = hwnd; }

    static int CharToIndex(char c)
    {
        c = (char)toupper((unsigned char)c);
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        if (c >= 'G' && c <= 'V') return 16 + (c - 'G');
        if (c >= 'W' && c <= 'Z') return 32 + (c - 'W');
        if (c == '+') return 36;
        if (c == ':') return 37;
        if (c == '*') return 38;
        if (c == '.') return 39;
        if (c == '!') return 40;
        return 41;
    }

    void Start(GraphicsContext* gfx) override
    {
        _CompileTexShader(gfx);
        _BuildFontTexture(gfx);

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        gfx->Device->CreateSamplerState(&sd, &sampler);

        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(TexVertex) * TEX_VB_MAX;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        gfx->Device->CreateBuffer(&bd, nullptr, &texVB);

        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(ConstantBuffer);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ConstantBuffer identity;
        identity.matWorld = XMMatrixTranspose(XMMatrixIdentity());
        D3D11_SUBRESOURCE_DATA isd = {};
        isd.pSysMem = &identity;
        gfx->Device->CreateBuffer(&cbd, &isd, &identityCB);

        texVerts.reserve(TEX_VB_MAX);
        printf("[UIText] Started.\n");
    }

    void Input()       override {}
    void Update(float) override {}

    void Render(GraphicsContext* gfx) override
    {
        if (gState == GameState::MAIN)
        {
            DrawString("DODGE++", 420, 200, 48, 64, { 1,1,1,1 });
            DrawString("BEST : " + std::to_string((int)gHighScore), 490, 290, 22, 32, { 0.4f,1,0.7f,1 });
            DrawString("SPACE  : START", 450, 350, 20, 28, { 0.8f,0.8f,0.8f,1 });
            DrawString("ARROW  : MOVE", 450, 390, 20, 28, { 0.8f,0.8f,0.8f,1 });
            DrawString("SPACE  : DASH 3S COOL", 450, 430, 20, 28, { 0.4f,0.86f,1,1 });
            DrawString("STAR   : INVINCIBLE", 450, 470, 20, 28, { 1,0.9f,0.2f,1 });
            DrawString("ESC    : QUIT", 450, 510, 20, 28, { 0.8f,0.8f,0.8f,1 });
        }
        else if (gState == GameState::PLAYING)
        {
            DrawString("SCORE : " + std::to_string((int)gScore), 10, 10, 20, 28, { 1,1,0.4f,1 });
            if (gIsInvincible)
                DrawString("INVINCIBLE " + std::to_string((int)(gInvTimer + 1.0f)) + "S",
                    SCREEN_W / 2 - 110, 10, 20, 28, { 1,0.86f,0.2f,1 });
            std::string label = (gDashCooldown <= 0.0f)
                ? "DASH READY"
                : ("DASH " + std::to_string((int)(gDashCooldown + 1.0f)) + "S");
            DrawString(label, (float)GAUGE_X, (float)(GAUGE_Y - 30), 18, 24, { 0.86f,0.86f,0.86f,1 });
        }
        else if (gState == GameState::GAMEOVER)
        {
            DrawString("GAME OVER", 400, 240, 44, 60, { 1,0.3f,0.3f,1 });
            DrawString("SCORE : " + std::to_string((int)gScore), 490, 330, 22, 32, { 1,1,0.4f,1 });
            DrawString("BEST  : " + std::to_string((int)gHighScore), 490, 370, 22, 32, { 0.4f,1,0.7f,1 });
            DrawString("R   : RESTART", 490, 420, 20, 28, { 0.8f,0.8f,0.8f,1 });
            DrawString("ESC : QUIT", 490, 460, 20, 28, { 0.8f,0.8f,0.8f,1 });
            DrawString("M   : MENU", 490, 500, 20, 28, { 0.7f,0.86f,1,1 });
        }
        _FlushText(gfx);
    }

    ~UITextRenderer() override
    {
        if (fontSRV) { fontSRV->Release();    fontSRV = nullptr; }
        if (sampler) { sampler->Release();    sampler = nullptr; }
        if (texVB) { texVB->Release();      texVB = nullptr; }
        if (identityCB) { identityCB->Release(); identityCB = nullptr; }
        if (texVS) { texVS->Release();      texVS = nullptr; }
        if (texPS) { texPS->Release();      texPS = nullptr; }
        if (texLayout) { texLayout->Release();  texLayout = nullptr; }
    }

private:
    void DrawString(const std::string& text, float x, float y,
        float charW, float charH, XMFLOAT4 col)
    {
        float cx = x;
        for (char c : text)
        {
            if (c == ' ') { cx += charW * 0.6f; continue; }
            int idx = CharToIndex(c), cellX = idx % FONT_COLS, cellY = idx / FONT_COLS;
            float u0 = (cellX * FONT_CELL_W) / (float)FONT_TEX_W, v0 = (cellY * FONT_CELL_H) / (float)FONT_TEX_H;
            float u1 = ((cellX + 1) * FONT_CELL_W) / (float)FONT_TEX_W, v1 = ((cellY + 1) * FONT_CELL_H) / (float)FONT_TEX_H;
            float x0n = PxToNdcX(cx), y0n = PxToNdcY(y);
            float x1n = PxToNdcX(cx + charW), y1n = PxToNdcY(y + charH);
            texVerts.push_back({ {x0n,y0n,0},{u0,v0},col });
            texVerts.push_back({ {x1n,y0n,0},{u1,v0},col });
            texVerts.push_back({ {x1n,y1n,0},{u1,v1},col });
            texVerts.push_back({ {x0n,y0n,0},{u0,v0},col });
            texVerts.push_back({ {x1n,y1n,0},{u1,v1},col });
            texVerts.push_back({ {x0n,y1n,0},{u0,v1},col });
            cx += charW;
        }
    }

    void _FlushText(GraphicsContext* gfx)
    {
        if (texVerts.empty() || !texVB || !fontSRV) return;
        auto* ctx = gfx->ImmediateContext;
        ctx->IASetInputLayout(texLayout);
        ctx->VSSetShader(texVS, nullptr, 0);
        ctx->PSSetShader(texPS, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &identityCB);
        ctx->PSSetShaderResources(0, 1, &fontSRV);
        ctx->PSSetSamplers(0, 1, &sampler);
        D3D11_MAPPED_SUBRESOURCE ms = {};
        if (SUCCEEDED(ctx->Map(texVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        {
            size_t cnt = min(texVerts.size(), (size_t)TEX_VB_MAX);
            memcpy(ms.pData, texVerts.data(), sizeof(TexVertex) * cnt);
            ctx->Unmap(texVB, 0);
        }
        UINT stride = sizeof(TexVertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &texVB, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw((UINT)min(texVerts.size(), (size_t)TEX_VB_MAX), 0);
        texVerts.clear();
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void _BuildFontTexture(GraphicsContext* gfx)
    {
        const int W = FONT_TEX_W, H = FONT_TEX_H;
        HDC     screenDC = GetDC(hWnd);
        HDC     memDC = CreateCompatibleDC(screenDC);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = W; bmi.bmiHeader.biHeight = -H;
        bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);
        memset(bits, 0, W * H * 4);
        HFONT hFont = CreateFont(FONT_CELL_H - 6, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        SelectObject(memDC, hFont);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 255));
        const char* CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ+:*.! ";
        for (int i = 0; CHARS[i] != '\0'; ++i)
        {
            int col = i % FONT_COLS, row = i / FONT_COLS;
            wchar_t wc[2] = { (wchar_t)(unsigned char)CHARS[i],0 };
            TextOutW(memDC, col * FONT_CELL_W + 2, row * FONT_CELL_H + 2, wc, 1);
        }
        std::vector<uint32_t> rgba(W * H);
        BYTE* src = (BYTE*)bits;
        for (int i = 0; i < W * H; ++i)
        {
            BYTE b = src[i * 4], g = src[i * 4 + 1], r = src[i * 4 + 2];
            BYTE a = (BYTE)(((int)r + g + b) / 3);
            rgba[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA tsd = {};
        tsd.pSysMem = rgba.data(); tsd.SysMemPitch = W * 4;
        ID3D11Texture2D* tex = nullptr;
        gfx->Device->CreateTexture2D(&td, &tsd, &tex);
        gfx->Device->CreateShaderResourceView(tex, nullptr, &fontSRV);
        tex->Release();
        // GDI 즉시 해제 — 이후 GDI 미사용
        SelectObject(memDC, oldBmp);
        DeleteObject(hFont); DeleteObject(hBmp);
        DeleteDC(memDC); ReleaseDC(hWnd, screenDC);
    }

    void _CompileTexShader(GraphicsContext* gfx)
    {
        ID3DBlob* vsBlob = nullptr, * psBlob = nullptr, * errBlob = nullptr;
        D3DCompile(TEX_SHADER_SRC.c_str(), TEX_SHADER_SRC.size(), nullptr, nullptr, nullptr,
            "VS", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }
        D3DCompile(TEX_SHADER_SRC.c_str(), TEX_SHADER_SRC.size(), nullptr, nullptr, nullptr,
            "PS", "ps_5_0", 0, 0, &psBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }
        if (!vsBlob || !psBlob) return;
        gfx->Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &texVS);
        gfx->Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &texPS);
        D3D11_INPUT_ELEMENT_DESC ied[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,      0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,20,D3D11_INPUT_PER_VERTEX_DATA,0},
        };
        gfx->Device->CreateInputLayout(ied, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &texLayout);
        vsBlob->Release(); psBlob->Release();
    }
};

const std::string UITextRenderer::TEX_SHADER_SRC = R"(
    cbuffer cbWorld : register(b0) { matrix matWorld; }
    Texture2D    gFont   : register(t0);
    SamplerState gSampler: register(s0);
    struct VS_IN { float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR; };
    struct PS_IN { float4 pos:SV_POSITION; float2 uv:TEXCOORD; float4 col:COLOR; };
    PS_IN VS(VS_IN i)
    {
        PS_IN o;
        o.pos=mul(float4(i.pos,1.0f),matWorld);
        o.uv=i.uv; o.col=i.col;
        return o;
    }
    float4 PS(PS_IN i):SV_Target
    {
        float4 tex=gFont.Sample(gSampler,i.uv);
        return float4(i.col.rgb, i.col.a*tex.a);
    }
)";

// ============================================================
// GameManagerComponent  (신규 Component)
// 역할  : 장애물·아이템 스폰 / 충돌 판정 / 난이도 관리
// 부착처: gSystemObj
// ResetGame() : GameLoop::Input 에서 직접 호출
// ============================================================
class GameManagerComponent : public Component
{
    std::mt19937 rng;
    std::uniform_real_distribution<float> randPosX;
    std::uniform_real_distribution<float> randPosY;
    std::uniform_int_distribution<int>    randSide{ 0,3 };
    std::uniform_real_distribution<float> randDiag{ -1.0f,1.0f };

    float spawnTimer = 0.0f;
    float itemSpawnTimer = 0.0f;
    int   difficultyLevel = 0;
    float difficultyTimer = 0.0f;

public:
    // 외부 주입 (Start 전에 설정)
    std::vector<GameObject*>* obstacles = nullptr;
    std::vector<GameObject*>* items = nullptr;
    GameObject* player = nullptr;
    GraphicsContext* gfxRef = nullptr;

    Mesh* obsMesh = nullptr;
    Material* obsMat = nullptr;
    Mesh* itemMesh = nullptr;
    Material* itemMat = nullptr;

    GameManagerComponent()
        : rng(std::random_device{}())
        , randPosX(ITEM_HALF_W + 50, SCREEN_W - ITEM_HALF_W - 50)
        , randPosY(ITEM_HALF_H + 50, SCREEN_H - ITEM_HALF_H - 50)
    {}

    void Start(GraphicsContext* gfx) override
    {
        gfxRef = gfx;
        printf("[GameManager] Started.\n");
    }

    void Input() override {}

    void Update(float dt) override
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

        // 아이템 획득 판정
        if (player && player->active && items)
            for (auto* it : *items)
            {
                if (!it->active) continue;
                if (CheckAABB(*player, *it))
                {
                    it->active = false; gIsInvincible = true; gInvTimer = INVINCIBLE_DURATION;
                    printf("[Invincible] 무적 획득! %.1f초\n", INVINCIBLE_DURATION);
                }
            }

        // 충돌 판정
        if (!gIsInvincible && player && player->active && obstacles)
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

    void Render(GraphicsContext*) override {}

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
        printf("[Game] Reset.\n");
    }

private:
    float GetSpawnInterval() const
    {
        return OBS_SPAWN_INTERVAL * (1.0f - ((float)difficultyLevel / DIFFICULTY_MAX) * 0.67f);
    }
    void GetSpeedRange(float& mn, float& mx) const
    {
        mn = OBS_SPEED_MIN + difficultyLevel * 40.0f;
        mx = OBS_SPEED_MAX + difficultyLevel * 60.0f;
    }
    int GetSpawnCount() const { return 4 + difficultyLevel; }

    void CalcObsSpawn(float& ox, float& oy, float& ovx, float& ovy)
    {
        float sMin, sMax; GetSpeedRange(sMin, sMax);
        std::uniform_real_distribution<float> dynSpeed(sMin, sMax);
        int side = randSide(rng); float speed = dynSpeed(rng), diag = randDiag(rng);
        switch (side)
        {
        case 0: ox = randPosX(rng); oy = -OBS_HALF_H;         ovx = speed * diag; ovy = speed;  break;
        case 1: ox = randPosX(rng); oy = SCREEN_H + OBS_HALF_H; ovx = speed * diag; ovy = -speed; break;
        case 2: ox = -OBS_HALF_W;   oy = randPosY(rng);        ovx = speed;     ovy = speed * diag; break;
        default:ox = SCREEN_W + OBS_HALF_W; oy = randPosY(rng); ovx = -speed;    ovy = speed * diag; break;
        }
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
                    float x, y, vx, vy; CalcObsSpawn(x, y, vx, vy);
                    obj->posX = x; obj->posY = y; obj->active = true;
                    auto* ctrl = dynamic_cast<ObstacleController*>(obj->components[0]);
                    if (ctrl) { ctrl->velX = vx; ctrl->velY = vy; }
                    reused = true; break;
                }
            }
            if (reused) continue;
            if ((int)obstacles->size() >= OBS_MAX) break;
            float x, y, vx, vy; CalcObsSpawn(x, y, vx, vy);
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
        if (!items || !gfxRef || !itemMesh || !itemMat) return;
        for (auto* it : *items) if (it->active) return;
        float x = randPosX(rng), y = randPosY(rng);
        for (auto* it : *items)
        {
            if (!it->active) { it->posX = x; it->posY = y; it->active = true; return; }
        }
        auto* item = new GameObject(x, y, ITEM_HALF_W, ITEM_HALF_H);
        item->AddComponent(new MeshRenderer(itemMesh, itemMat));
        items->push_back(item);
        printf("[Item] 무적 스폰 (%.0f,%.0f)\n", x, y);
    }

    void UpdateInvincible(float dt)
    {
        if (!gIsInvincible) return;
        gInvTimer -= dt;
        if (gInvTimer <= 0.0f) { gIsInvincible = false; gInvTimer = 0.0f; printf("[Invincible] 종료\n"); }
    }
};

// ============================================================
// GameLoop
// 씬 오브젝트
//   world      : player 등 영속
//   obstacles  : 장애물 풀
//   items      : 아이템 풀
//   gSystemObj : GameManagerComponent 부착
//   gUIObj     : StarBackgroundRenderer 부착
//   gUITextObj : UITextRenderer 부착 (Flush 이후 Draw)
// ============================================================
class GameLoop
{
public:
    WindowContext            win;
    GraphicsContext          gfx;
    DeltaTime                timer;
    bool                     isRunning = true;

    std::vector<GameObject*> world;
    std::vector<GameObject*> obstacles;
    std::vector<GameObject*> items;

    // 시스템 오브젝트 (소유)
    GameObject* gSystemObj = nullptr; // GameManagerComponent
    GameObject* gUIObj = nullptr; // StarBackgroundRenderer
    GameObject* gUITextObj = nullptr; // UITextRenderer

    GameManagerComponent* managerComp = nullptr; // 빠른 접근

    GameLoop() { printf("[Engine] GameLoop Created.\n"); }
    ~GameLoop()
    {
        for (auto* o : world)     delete o;
        for (auto* o : obstacles) delete o;
        for (auto* o : items)     delete o;
        delete gSystemObj;
        delete gUIObj;
        delete gUITextObj;
        printf("[Engine] GameLoop Destroyed.\n");
    }

    void Initialize(HINSTANCE hInst, LRESULT(CALLBACK* wndProc)(HWND, UINT, WPARAM, LPARAM))
    {
        win.Initialize(hInst, wndProc);
        gfx.InitDX(win.hWnd, SCREEN_W, SCREEN_H);
    }

    void Input()
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) isRunning = false;

        if (gState == GameState::MAIN && (GetAsyncKeyState(VK_SPACE) & 0x0001))
        {
            gState = GameState::PLAYING; if (managerComp) managerComp->ResetGame();
        }

        if (gState == GameState::GAMEOVER && (GetAsyncKeyState('R') & 0x0001))
        {
            gState = GameState::PLAYING; if (managerComp) managerComp->ResetGame();
        }

        if (gState == GameState::GAMEOVER && (GetAsyncKeyState('M') & 0x0001))
        {
            gState = GameState::MAIN; if (managerComp) managerComp->ResetGame();
        }

        // 씬 전체 Input 순회
        if (gSystemObj) gSystemObj->Input();
        if (gUIObj)     gUIObj->Input();
        if (gUITextObj) gUITextObj->Input();
        for (auto* obj : world) if (obj && obj->active) obj->Input();
    }

    void Update(float dt)
    {
        if (gSystemObj) gSystemObj->Update(dt, &gfx);
        if (gUIObj)     gUIObj->Update(dt, &gfx);
        if (gUITextObj) gUITextObj->Update(dt, &gfx);
        for (auto* obj : world)     if (obj && obj->active) obj->Update(dt, &gfx);
        for (auto* obs : obstacles) if (obs && obs->active) obs->Update(dt, &gfx);
        for (auto* it : items)     if (it && it->active)  it->Update(dt, &gfx);
    }

    void Render()
    {
        // 1. Clear
        float col[] = { 0.0f,0.0f,0.05f,1.0f };
        gfx.ImmediateContext->ClearRenderTargetView(gfx.RTV, col);

        D3D11_VIEWPORT vp = { 0,0,(float)SCREEN_W,(float)SCREEN_H,0,1 };
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
        float blendFactor[4] = {};
        gfx.ImmediateContext->OMSetBlendState(blendState, blendFactor, 0xffffffff);
        blendState->Release();

        // 2. 별 배경 → Batch 누적
        if (gUIObj) gUIObj->Render(&gfx);

        // 3. 정적 Mesh
        for (auto* obs : obstacles) if (obs && obs->active) obs->Render(&gfx);
        for (auto* it : items)     if (it && it->active)  it->Render(&gfx);
        for (auto* obj : world)     if (obj && obj->active) obj->Render(&gfx);

        // 4. PrimitiveBatch Flush (별·게이지·쉴드·잔상 일괄 Draw)
        gfx.Batch.Flush(gfx.ImmediateContext);

        // 5. UIText Draw (텍스처 쿼드, Flush 이후)
        if (gUITextObj) gUITextObj->Render(&gfx);

        // 6. Present
        gfx.SwapChain->Present(gfx.VSync, 0);
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
            else
            {
                float dt = timer.GetDelta();
                Input();
                Update(dt);
                Render();
            }
        }
    }
};

// ============================================================
// 공통 셰이더 소스
// ============================================================
static const std::string SHADER_SRC = R"(
    cbuffer cbWorld    : register(b0) { matrix matWorld;  }
    cbuffer cbMaterial : register(b1) { float4 tintColor; }
    struct VS_IN { float3 pos:POSITION; float4 col:COLOR; };
    struct PS_IN { float4 pos:SV_POSITION; float4 col:COLOR; };
    PS_IN VS(VS_IN i)
    {
        PS_IN o;
        o.pos=mul(float4(i.pos,1.0f),matWorld);
        o.col=i.col;
        return o;
    }
    float4 PS(PS_IN i):SV_Target { return i.col*tintColor; }
)";

// ============================================================
// Mesh 생성 헬퍼
// ============================================================
Mesh* CreateShipMesh(ID3D11Device* device, float r)
{
    float nW = (r / SCREEN_W) * 2, nH = (r / SCREEN_H) * 2;
    std::vector<Vertex> v = {
        {{ 0,    nH * 2,   0},{1,1,1,1}},{{ nW,  -nH,     0},{1,1,1,1}},{{-nW,  -nH,     0},{1,1,1,1}},
        {{ 0,   -nH * .5f,0},{1,1,1,1}},{{-nW,  -nH,     0},{1,1,1,1}},{{-nW * 2.5f,-nH * 1.5f,0},{1,1,1,1}},
        {{ 0,   -nH * .5f,0},{1,1,1,1}},{{ nW * 2.5f,-nH * 1.5f,0},{1,1,1,1}},{{ nW,-nH,    0},{1,1,1,1}},
    };
    Mesh* m = new Mesh(); m->Create(device, v); return m;
}

Mesh* CreateMeteorMesh(ID3D11Device* device, float r)
{
    const int P = 8;
    float nW = (r / SCREEN_W) * 2, nH = (r / SCREEN_H) * 2;
    XMFLOAT2 pts[P];
    for (int i = 0; i < P; ++i) { float a = i * XM_2PI / P; pts[i] = { cosf(a) * nW,sinf(a) * nH }; }
    std::vector<Vertex> v;
    for (int i = 0; i < P; ++i) {
        v.push_back({ {0,0,0},{1,1,1,1} });
        v.push_back({ {pts[(i + 1) % P].x,pts[(i + 1) % P].y,0},{1,1,1,1} });
        v.push_back({ {pts[i].x,pts[i].y,0},{1,1,1,1} });
    }
    Mesh* m = new Mesh(); m->Create(device, v); return m;
}

Mesh* CreateStarMesh(ID3D11Device* device, float outerR, float innerR)
{
    float oX = (outerR / SCREEN_W) * 2, oY = (outerR / SCREEN_H) * 2;
    float iX = (innerR / SCREEN_W) * 2, iY = (innerR / SCREEN_H) * 2;
    XMFLOAT2 pts[10];
    for (int i = 0; i < 10; ++i) {
        float a = XM_PIDIV2 - (i * XM_2PI / 10);
        pts[i] = (i % 2 == 0) ? XMFLOAT2{ cosf(a) * oX,sinf(a) * oY } : XMFLOAT2{ cosf(a) * iX,sinf(a) * iY };
    }
    std::vector<Vertex> v;
    for (int i = 0; i < 10; ++i) {
        v.push_back({ {0,0,0},{1,1,1,1} });
        v.push_back({ {pts[i].x,pts[i].y,0},{1,1,1,1} });
        v.push_back({ {pts[(i + 1) % 10].x,pts[(i + 1) % 10].y,0},{1,1,1,1} });
    }
    Mesh* m = new Mesh(); m->Create(device, v); return m;
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
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,   0, 0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"COLOR",   0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}
    };

    ShaderSet shaders = gEngine.gfx.CompileAndCreate(SHADER_SRC, ied, 2);
    if (!shaders.vs || !shaders.ps) { printf("[Error] Shader compile failed.\n"); return -1; }
    if (!gEngine.gfx.Batch.Init(gEngine.gfx.Device, shaders)) { printf("[Error] Batch init failed.\n"); return -1; }

    Mesh* playerMesh = CreateShipMesh(gEngine.gfx.Device, 15.0f);
    Mesh* obsMesh = CreateMeteorMesh(gEngine.gfx.Device, 15.0f);
    Mesh* itemMesh = CreateStarMesh(gEngine.gfx.Device, ITEM_HALF_W, ITEM_HALF_W * 0.42f);

    ColorMaterial* playerMat = new ColorMaterial(shaders, { 0.3f,0.8f,1.0f,1.0f }, gEngine.gfx.Device);
    ColorMaterial* obsMat = new ColorMaterial(shaders, { 1.0f,0.5f,0.2f,1.0f }, gEngine.gfx.Device);
    ColorMaterial* itemMat = new ColorMaterial(shaders, { 1.0f,0.9f,0.1f,1.0f }, gEngine.gfx.Device);

    // ── 플레이어 ─────────────────────────────────────────────
    auto* player = new GameObject(SCREEN_W / 2.0f, SCREEN_H / 2.0f, PLAYER_HALF_W, PLAYER_HALF_H);
    auto* trail = new DashTrailRenderer();
    player->AddComponent(new MeshRenderer(playerMesh, playerMat)); // [0]
    player->AddComponent(new PlayerController());                  // [1]
    player->AddComponent(new BlinkRenderer(playerMat));            // [2]
    player->AddComponent(new CooldownBarRenderer());               // [3]
    player->AddComponent(new ShieldRenderer());                    // [4]
    player->AddComponent(trail);                                   // [5]
    auto* pc = dynamic_cast<PlayerController*>(player->components[1]);
    if (pc) pc->pTrail = trail;
    gEngine.world.push_back(player);

    // ── gSystemObj : GameManagerComponent ────────────────────
    gEngine.gSystemObj = new GameObject();
    auto* managerComp = new GameManagerComponent();
    managerComp->player = player;
    managerComp->obstacles = &gEngine.obstacles;
    managerComp->items = &gEngine.items;
    managerComp->obsMesh = obsMesh;
    managerComp->obsMat = obsMat;
    managerComp->itemMesh = itemMesh;
    managerComp->itemMat = itemMat;
    gEngine.gSystemObj->AddComponent(managerComp);
    gEngine.managerComp = managerComp;

    // ── gUIObj : StarBackgroundRenderer ──────────────────────
    gEngine.gUIObj = new GameObject();
    gEngine.gUIObj->AddComponent(new StarBackgroundRenderer());

    // ── gUITextObj : UITextRenderer ──────────────────────────
    gEngine.gUITextObj = new GameObject();
    auto* uiText = new UITextRenderer();
    uiText->SetHWND(gEngine.win.hWnd);
    gEngine.gUITextObj->AddComponent(uiText);

    printf("[Game] 메인화면 | SPACE: 시작 / ESC: 종료\n");

    gEngine.Run();

    delete playerMat; delete obsMat; delete itemMat;
    delete playerMesh; delete obsMesh; delete itemMesh;
    shaders.Release();

    return 0;
}
