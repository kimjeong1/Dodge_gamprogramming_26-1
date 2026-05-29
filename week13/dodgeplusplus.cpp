/*
================================================================================
 Dodge++
 13주차 : 완성도 올리기 — GDI 완전 제거 + 움직이는 별 배경 + 4방향 대시
--------------------------------------------------------------------------------
 [13주차 변경 사항]

 A (로직)
   1. 대시 4방향 제한
      - TryDash() 에서 lastDir 의 x/y 크기를 비교해 더 큰 축만 사용
      - 사선 대시 완전 차단
   2. 별 배경 스크롤
      - Star 구조체에 vy(낙하 속도) 추가
      - TextRenderer::Update() 에서 매 프레임 y 증가, 화면 밖 나가면 위로 순환
   3. 대시 직후 재발동 버그 수정
      - ResetGame() 에서 GetAsyncKeyState(VK_SPACE) 호출로 잔여 입력 소비

 B (렌더링)
   4. GDI 완전 제거
      - TextRenderer 텍스트 → 런타임 비트맵 폰트 + DirectX 텍스처 매핑
      - 초기화 1회만 GDI 사용(폰트 비트맵 생성) → 즉시 D3D11 텍스처로 변환 후 GDI 해제
      - 매 프레임 렌더링은 100% DirectX
   5. 텍스트 UI 영어로 전환
      - DODGE++, SCORE, BEST, DASH READY, INVINCIBLE, GAME OVER 등

 렌더링 파이프라인 구조
  ┌─ 매 프레임 Render() 흐름 ─────────────────────────────────────────┐
  │  1. ClearRenderTargetView (배경 초기화)                            │
  │  2. text.RenderStars()   → PrimitiveBatch 에 별 사각형 누적        │
  │  3. MeshRenderer::Render() → 장애물·아이템·플레이어 정적 Mesh Draw │
  │  4. 컴포넌트 Render()    → DashTrail·Cooldown·Shield Batch 누적   │
  │  5. Batch.Flush()        → TRIANGLELIST / LINELIST 일괄 GPU 제출  │
  │  6. text.RenderUI()      → 비트맵 폰트 텍스처 쿼드 Draw            │
  │  7. SwapChain::Present() → 화면 출력 (GDI 오버레이 없음)           │
  └────────────────────────────────────────────────────────────────────┘

 ┌─ PrimitiveBatch ───────────────────────────────────────────────────┐
 │  CPU 버텍스 배열을 매 프레임 Dynamic Buffer 에 Map/Unmap            │
 │  TRIANGLELIST(삼각형·사각형) / LINELIST(선·외곽선) 두 토폴로지 지원 │
 │  AddTri / AddRect / AddLine / AddPolyline → Flush() 패턴          │
 └────────────────────────────────────────────────────────────────────┘

 ┌─ 비트맵 폰트 (TextRenderer) ──────────────────────────────────────┐
 │  Initialize() 1회:                                                 │
 │    GDI 메모리 DC → 0~9, A~Z, 특수문자 그리기                       │
 │    → 픽셀 데이터 RGBA 변환 → D3D11Texture2D 업로드 → GDI 해제      │
 │  매 프레임 RenderUI():                                              │
 │    DrawString() → UV 계산 → TexVertex 쿼드 누적 → _FlushText()    │
 └────────────────────────────────────────────────────────────────────┘

 클래스 구조
  Component (추상)
    ├─ MeshRenderer        : 정적 Mesh + Material 로 오브젝트 렌더링
    ├─ PlayerController    : 키보드 입력·이동·대시 처리
    ├─ BlinkRenderer       : 무적 중 플레이어 색상 깜빡임
    ├─ ObstacleController  : 장애물 이동 및 화면 밖 비활성화
    ├─ CooldownBarRenderer : 대시 쿨타임 게이지 (PrimitiveBatch)
    ├─ ShieldRenderer      : 무적 중 회전 육각형 쉴드 (PrimitiveBatch)
    └─ DashTrailRenderer   : 대시 잔상·파티클 이펙트 (PrimitiveBatch)
  Material (추상)
    └─ ColorMaterial       : 단색 tintColor 를 b1 슬롯에 전송
  GameObject               : Component 리스트 보유, 위치·크기·활성 상태
  GameManager              : 장애물·아이템 스폰, 충돌 판정, 난이도 관리
  GameLoop                 : 메인 루프 (Input → Update → Render)
  TextRenderer             : 별 배경 + 비트맵 폰트 UI 렌더링
  GraphicsContext          : D3D11 디바이스·스왑체인·RTV + PrimitiveBatch
  WindowContext            : Win32 윈도우 생성 (크기 고정)
  DeltaTime                : 프레임 독립 delta time 계산

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
// 게임 전체에서 공유하는 수치를 한 곳에 정의
// 수치 변경 시 이 블록만 수정하면 됨
// ============================================================

// 화면 해상도
static const int   SCREEN_W = 1280;
static const int   SCREEN_H = 720;

// 플레이어 이동 속도 및 충돌 박스 반크기 (픽셀)
static const float PLAYER_SPEED = 500.0f;
static const float PLAYER_HALF_W = 12.0f;
static const float PLAYER_HALF_H = 12.0f;

// 대시 : 1회 이동 거리(픽셀) / 재사용 대기 시간(초)
static const float DASH_DISTANCE = 180.0f;
static const float DASH_COOLDOWN = 3.0f;

// 장애물 충돌 박스 반크기 / 속도 범위 / 스폰 간격 / 풀 최대 개수
static const float OBS_HALF_W = 9.0f;
static const float OBS_HALF_H = 9.0f;
static const float OBS_SPEED_MIN = 150.0f;
static const float OBS_SPEED_MAX = 380.0f;
static const float OBS_SPAWN_INTERVAL = 1.2f;   // 초기 스폰 간격 (초)
static const int   OBS_MAX = 500;     // 오브젝트 풀 최대 크기

// 무적 아이템 충돌 박스 반크기 / 스폰 주기 / 무적 지속 시간
static const float ITEM_HALF_W = 14.0f;
static const float ITEM_HALF_H = 14.0f;
static const float ITEM_SPAWN_INTERVAL = 15.0f;
static const float INVINCIBLE_DURATION = 5.0f;

// 대시 쿨타임 게이지 UI 위치 및 크기 (픽셀)
static const int   GAUGE_X = 20;
static const int   GAUGE_Y = SCREEN_H - 40;
static const int   GAUGE_W = 160;
static const int   GAUGE_H = 18;

// 난이도 : 단계 상승 주기(초) / 최대 단계
static const float DIFFICULTY_INTERVAL = 20.0f;
static const int   DIFFICULTY_MAX = 6;

// ============================================================
// 전역 게임 상태
// 여러 컴포넌트·시스템에서 공유하는 런타임 상태값
// 읽기는 어디서든 허용, 쓰기는 GameManager 함수 경유 권장
// ============================================================
enum class GameState { MAIN, PLAYING, GAMEOVER };

GameState gState = GameState::MAIN; // 현재 화면 상태
float     gScore = 0.0f;            // 현재 생존 시간 (점수)
bool      gIsInvincible = false;           // 무적 여부
float     gInvTimer = 0.0f;            // 무적 남은 시간 (초)
float     gDashCooldown = 0.0f;            // 대시 남은 쿨타임 (초)
float     gHighScore = 0.0f;            // 세션 최고 점수

// ============================================================
// 기본 구조체
// ============================================================

// GPU 버텍스 레이아웃 : 위치(3D) + 색상(RGBA)
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT4 col;
};

// b0 슬롯 상수 버퍼 : 오브젝트별 월드 변환 행렬
struct ConstantBuffer { XMMATRIX matWorld; };

// b1 슬롯 상수 버퍼 : 머티리얼 색상 (tintColor)
struct ColorBuffer { XMFLOAT4 tintColor; };

// ============================================================
// NDC 변환 헬퍼
// 픽셀 좌표(좌상단 원점) → NDC 좌표(-1~1, 중앙 원점)
// DirectX NDC : X 오른쪽 +, Y 위쪽 +
// ============================================================
static inline float PxToNdcX(float px) { return (px / SCREEN_W) * 2.0f - 1.0f; }
static inline float PxToNdcY(float py) { return 1.0f - (py / SCREEN_H) * 2.0f; }

// ============================================================
// ShaderSet
// 컴파일된 VS·PS·InputLayout 을 하나의 단위로 묶음
// GraphicsContext::CompileAndCreate() 가 반환하며
// Material 과 PrimitiveBatch 가 공유해서 사용
// ============================================================
struct ShaderSet
{
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;

    // 소유한 GPU 리소스 해제
    void Release()
    {
        if (vs) { vs->Release();     vs = nullptr; }
        if (ps) { ps->Release();     ps = nullptr; }
        if (layout) { layout->Release(); layout = nullptr; }
    }
};

// ============================================================
// Mesh
// 정적 버텍스 데이터를 GPU 버퍼(D3D11_USAGE_DEFAULT)에 올림
// 생성 후 내용 변경 불가 → 동적 변경이 필요하면 PrimitiveBatch 사용
// ============================================================
class Mesh
{
public:
    ID3D11Buffer* vBuffer = nullptr;
    UINT          vertexCount = 0;

    // 소멸 시 GPU 버퍼 자동 해제
    ~Mesh() { if (vBuffer) { vBuffer->Release(); vBuffer = nullptr; } }

    // vertices 데이터를 GPU 에 업로드
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
// 매 프레임 CPU 에서 버텍스를 누적한 뒤 Flush() 에서
// Dynamic Buffer 에 한 번에 Map/Unmap 후 Draw
//
// 사용 패턴:
//   각 컴포넌트의 Render() 에서 AddTri / AddRect / AddLine 호출
//   → GameLoop::Render() 말미에 Flush() 한 번 호출
//
// TRIANGLELIST 와 LINELIST 를 별도 배열로 관리하여
// Flush() 에서 각각 한 번씩 Draw
// ============================================================
class PrimitiveBatch
{
    static const UINT MAX_VERTS = 65536; // 프레임당 최대 버텍스 수

    ID3D11Buffer* dynBuf = nullptr; // Dynamic 버텍스 버퍼
    ID3D11VertexShader* vs = nullptr; // 공유 셰이더 (ShaderSet 소유)
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11Buffer* cbWorld = nullptr; // b0: 항등 행렬 고정 (NDC 직접 사용)
    ID3D11Buffer* cbColor = nullptr; // b1: 흰색 고정 → 버텍스 col 패스스루

    std::vector<Vertex> triVerts;  // TRIANGLELIST 용 버텍스 누적 배열
    std::vector<Vertex> lineVerts; // LINELIST 용 버텍스 누적 배열

public:
    // 초기화 : Dynamic 버퍼·상수 버퍼 생성, 셰이더 포인터 저장
    bool Init(ID3D11Device* device, ShaderSet shaders)
    {
        // ShaderSet 포인터만 빌림 (소유권은 WinMain 의 ShaderSet 에 있음)
        vs = shaders.vs;
        ps = shaders.ps;
        layout = shaders.layout;

        // 매 프레임 CPU→GPU 업로드용 Dynamic 버텍스 버퍼 생성
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(Vertex) * MAX_VERTS;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &dynBuf))) return false;

        // b0 : 항등 행렬 (PrimitiveBatch 는 NDC 좌표를 직접 전달하므로 변환 불필요)
        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(ConstantBuffer);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ConstantBuffer identity;
        identity.matWorld = XMMatrixTranspose(XMMatrixIdentity());
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = &identity;
        device->CreateBuffer(&cbd, &sd, &cbWorld);

        // b1 : tintColor = (1,1,1,1) → 버텍스 col 색상이 그대로 출력됨
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
        // vs/ps/layout 는 ShaderSet 소유 → 여기서 해제하지 않음
    }

    // ── 도형 누적 함수 (픽셀 좌표 입력) ──────────────────────

    // 삼각형 1개 추가
    void AddTri(XMFLOAT2 a, XMFLOAT2 b, XMFLOAT2 c, XMFLOAT4 col)
    {
        if (triVerts.size() + 3 > MAX_VERTS) return;
        triVerts.push_back({ { PxToNdcX(a.x), PxToNdcY(a.y), 0 }, col });
        triVerts.push_back({ { PxToNdcX(b.x), PxToNdcY(b.y), 0 }, col });
        triVerts.push_back({ { PxToNdcX(c.x), PxToNdcY(c.y), 0 }, col });
    }

    // 채워진 사각형 추가 (삼각형 2개로 분할)
    void AddRect(float x0, float y0, float x1, float y1, XMFLOAT4 col)
    {
        AddTri({ x0,y0 }, { x1,y0 }, { x1,y1 }, col);
        AddTri({ x0,y0 }, { x1,y1 }, { x0,y1 }, col);
    }

    // 선분 1개 추가
    void AddLine(XMFLOAT2 a, XMFLOAT2 b, XMFLOAT4 col)
    {
        if (lineVerts.size() + 2 > MAX_VERTS) return;
        lineVerts.push_back({ { PxToNdcX(a.x), PxToNdcY(a.y), 0 }, col });
        lineVerts.push_back({ { PxToNdcX(b.x), PxToNdcY(b.y), 0 }, col });
    }

    // 다각형 외곽선 추가 (점 배열 → 선분 연결, close=true 이면 마지막-첫 점 연결)
    void AddPolyline(const std::vector<XMFLOAT2>& pts, XMFLOAT4 col, bool close = true)
    {
        int n = (int)pts.size();
        for (int i = 0; i < n - 1; ++i)
            AddLine(pts[i], pts[i + 1], col);
        if (close && n > 1)
            AddLine(pts[n - 1], pts[0], col); // 닫힌 다각형
    }

    // ── 일괄 GPU 제출 ─────────────────────────────────────────
    // GameLoop::Render() 말미에 한 번만 호출
    // TRIANGLELIST → LINELIST 순서로 각각 Upload → Draw
    void Flush(ID3D11DeviceContext* ctx)
    {
        if (triVerts.empty() && lineVerts.empty()) return;

        // 공유 셰이더·레이아웃·상수 버퍼 바인딩
        ctx->IASetInputLayout(layout);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cbWorld);
        ctx->PSSetConstantBuffers(1, 1, &cbColor);

        UINT stride = sizeof(Vertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &dynBuf, &stride, &offset);

        // 채워진 도형 (삼각형·사각형·잔상·파티클·별)
        if (!triVerts.empty())
        {
            Upload(ctx, triVerts);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->Draw((UINT)triVerts.size(), 0);
            triVerts.clear();
        }

        // 외곽선 (쉴드·게이지 테두리)
        if (!lineVerts.empty())
        {
            Upload(ctx, lineVerts);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            ctx->Draw((UINT)lineVerts.size(), 0);
            lineVerts.clear();
        }

        // 다음 패스가 기본 토폴로지를 가정할 수 있도록 복원
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

private:
    // dynBuf 에 버텍스 데이터를 Map/Unmap 으로 업로드
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
// Material (추상 기저 클래스)
// ShaderSet 을 보유하고, Bind() 에서 셰이더·상수 버퍼를 파이프라인에 설정
// ColorMaterial 이 유일한 구현체 (단색 렌더링)
// ============================================================
class Material
{
public:
    ShaderSet shaders;
    Material(ShaderSet s) : shaders(s) {}
    virtual ~Material() {}

    // 파이프라인에 셰이더·상수 버퍼를 바인딩 (순수 가상)
    virtual void Bind(ID3D11DeviceContext* context) = 0;
};

// ============================================================
// ColorMaterial
// b1 슬롯에 tintColor 를 전송하여 오브젝트를 단색으로 렌더링
// BlinkRenderer 가 SetColor() 로 색상을 실시간 변경함
// ============================================================
class ColorMaterial : public Material
{
public:
    XMFLOAT4      color;         // 현재 색상 (RGBA 0~1)
    ID3D11Buffer* pColorBuffer = nullptr; // b1 상수 버퍼

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

    // 색상 변경 (BlinkRenderer 에서 호출)
    void SetColor(XMFLOAT4 col) { color = col; }

    // 셰이더·레이아웃·tintColor 상수 버퍼를 파이프라인에 바인딩
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
// 프레임 독립 처리를 위한 경과 시간 계산
// 최대 0.05초(50ms) 로 클램프 → 디버거 중단 후 재개 시 튀는 현상 방지
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
        if (dt > 0.05f) dt = 0.05f; // 최대 50ms 클램프
        return dt;
    }
};

// ============================================================
// WindowContext
// Win32 윈도우 생성 및 관리
// WS_THICKFRAME·WS_MAXIMIZEBOX 제거 → 크기 고정 윈도우
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

        // WS_THICKFRAME / WS_MAXIMIZEBOX 제외 → 크기 변경·최대화 불가
        hWnd = CreateWindow(L"DX11Engine", L"Dodge++ | Week 13",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            NULL, NULL, hInst, NULL);
        if (!hWnd) return false;

        ShowWindow(hWnd, SW_SHOW);
        printf("[Engine] Window Created. (%dx%d)\n", Width, Height);
        return true;
    }
};

// ============================================================
// GraphicsContext
// D3D11 디바이스·스왑체인·렌더타겟 초기화 및 셰이더 컴파일
// PrimitiveBatch 를 멤버로 보유하여 컴포넌트에서 gfx->Batch 로 접근
// ============================================================
class GraphicsContext
{
public:
    ID3D11Device* Device = nullptr;
    ID3D11DeviceContext* ImmediateContext = nullptr;
    IDXGISwapChain* SwapChain = nullptr;
    ID3D11RenderTargetView* RTV = nullptr;
    int                     VSync = 1; // 수직동기화 (1=ON)

    // GDI 대체 즉석 렌더러 — 컴포넌트들이 gfx->Batch.AddXxx() 로 도형 누적
    PrimitiveBatch Batch;

    // D3D11 디바이스·스왑체인 생성 및 RTV 초기화
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

    // 스왑체인 백버퍼에서 RTV 생성 (리사이즈 시 재생성에도 사용)
    void CreateRTV()
    {
        if (RTV) RTV->Release();
        ID3D11Texture2D* pBB = nullptr;
        SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBB);
        Device->CreateRenderTargetView(pBB, NULL, &RTV);
        pBB->Release();
    }

    // HLSL 소스 문자열을 컴파일하여 ShaderSet 반환
    ShaderSet CompileAndCreate(const std::string& src,
        D3D11_INPUT_ELEMENT_DESC* ied, UINT iedCount)
    {
        ShaderSet res;
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errBlob = nullptr;

        // VS 컴파일
        D3DCompile(src.c_str(), src.length(), nullptr, nullptr, nullptr,
            "VS", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }

        // PS 컴파일
        D3DCompile(src.c_str(), src.length(), nullptr, nullptr, nullptr,
            "PS", "ps_5_0", 0, 0, &psBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }

        if (!vsBlob || !psBlob) return res;

        Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &res.vs);
        Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &res.ps);

        if (ied && iedCount > 0)
            Device->CreateInputLayout(ied, iedCount,
                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &res.layout);

        vsBlob->Release();
        psBlob->Release();
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
// Component (순수 추상 기저 클래스)
// GameObject 가 컴포넌트 리스트를 보유하고 순회 호출
// pOwner : 이 컴포넌트가 붙어있는 GameObject 포인터
// isStarted : 첫 Update 전에 Start() 가 한 번 호출됨
// ============================================================
class GameObject; // 전방 선언

class Component
{
public:
    GameObject* pOwner = nullptr;
    bool        isStarted = false;

    virtual void Start(GraphicsContext* gfx) = 0; // 첫 프레임 초기화
    virtual void Input() = 0; // 입력 처리
    virtual void Update(float dt) = 0; // 매 프레임 로직
    virtual void Render(GraphicsContext* gfx) = 0; // 매 프레임 렌더링
    virtual ~Component() {}
};

// ============================================================
// GameObject
// 위치(posX/Y)·크기(halfW/H)·활성 여부를 가진 씬 오브젝트
// Component 리스트를 보유하여 Update/Render 를 위임
// ============================================================
class GameObject
{
public:
    float posX = 0.0f;
    float posY = 0.0f;
    float halfW = 0.0f; // AABB 충돌 박스 반너비
    float halfH = 0.0f; // AABB 충돌 박스 반높이
    bool  active = true; // false 이면 Update·Render 건너뜀 (오브젝트 풀 재사용)
    std::vector<Component*> components;

    GameObject(float x, float y, float hw = 0, float hh = 0)
        : posX(x), posY(y), halfW(hw), halfH(hh) {}

    // 소멸 시 모든 컴포넌트 해제
    ~GameObject() { for (auto* c : components) delete c; }

    // 컴포넌트 추가 및 pOwner 연결
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
            // 첫 Update 호출 전 Start() 실행
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
// CheckAABB
// 두 GameObject 의 축정렬 경계 박스(AABB) 충돌 여부 반환
// ============================================================
bool CheckAABB(const GameObject& a, const GameObject& b)
{
    return (a.posX - a.halfW < b.posX + b.halfW &&
        a.posX + a.halfW > b.posX - b.halfW &&
        a.posY - a.halfH < b.posY + b.halfH &&
        a.posY + a.halfH > b.posY - b.halfH);
}

// ============================================================
// MeshRenderer
// 정적 Mesh + Material 로 GameObject 를 렌더링하는 컴포넌트
// b0 에 오브젝트 월드 행렬(위치 이동)을 전송
// b1 은 Material::Bind() 가 처리
// ============================================================
class MeshRenderer : public Component
{
    Mesh* pMeshData = nullptr;
    Material* pMaterial = nullptr;
    ID3D11Buffer* cBuffer = nullptr; // b0 상수 버퍼 (월드 행렬)

public:
    MeshRenderer(Mesh* mesh, Material* mat) : pMeshData(mesh), pMaterial(mat) {}

    ~MeshRenderer() override
    {
        if (cBuffer) { cBuffer->Release(); cBuffer = nullptr; }
    }

    // 첫 프레임에 b0 상수 버퍼 생성
    void Start(GraphicsContext* gfx) override
    {
        D3D11_BUFFER_DESC cbd = {};
        cbd.Usage = D3D11_USAGE_DEFAULT;
        cbd.ByteWidth = sizeof(ConstantBuffer);
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        gfx->Device->CreateBuffer(&cbd, nullptr, &cBuffer);
    }

    // pOwner 의 픽셀 위치를 NDC 이동 행렬로 변환하여 b0 에 전송 후 Draw
    void Render(GraphicsContext* gfx) override
    {
        if (!pMeshData || !pMaterial) return;

        // b1 (색상) + 셰이더 바인딩
        pMaterial->Bind(gfx->ImmediateContext);

        // 픽셀 좌표 → NDC 이동 행렬 (Mesh 버텍스는 오브젝트 공간 NDC 단위)
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
// DashAfterimage / DashParticle 구조체
// DashTrailRenderer 가 내부적으로 관리하는 이펙트 데이터
// ============================================================

// 대시 잔상 : 함선 실루엣을 반투명하게 남김
struct DashAfterimage
{
    float x, y;       // 잔상 중심 위치 (픽셀)
    float alpha;      // 현재 불투명도 (0~0.75)
    float lifetime;   // 남은 수명 (초)
};

// 대시 파티클 : 대시 시작점에서 역방향으로 흩뿌려지는 작은 사각형
struct DashParticle
{
    float x, y;       // 현재 위치 (픽셀)
    float vx, vy;     // 이동 속도 (픽셀/초)
    float alpha;      // 현재 불투명도
    float lifetime;   // 남은 수명 (초)
    float size;       // 사각형 반크기 (픽셀)
};

// ============================================================
// DashTrailRenderer
// 대시 발동 시 잔상(함선 실루엣)과 파티클 이펙트를 생성·업데이트·렌더링
// PrimitiveBatch 의 AddTri / AddRect 로 DirectX 렌더링
// ============================================================
class DashTrailRenderer : public Component
{
    std::vector<DashAfterimage> afterimages; // 활성 잔상 목록
    std::vector<DashParticle>   particles;   // 활성 파티클 목록

    std::mt19937                          rng;
    std::uniform_real_distribution<float> randAngle; // 파티클 방향 (0~2π)
    std::uniform_real_distribution<float> randSpeed; // 파티클 속도
    std::uniform_real_distribution<float> randSize;  // 파티클 크기

    // 대시 시작·끝 위치 및 방향 (OnDash 에서 저장)
    float dashDirX = 0.0f, dashDirY = 0.0f;
    float dashStartX = 0.0f, dashStartY = 0.0f;
    float dashEndX = 0.0f, dashEndY = 0.0f;

    // 잔상 연속 스폰 상태
    bool  spawningAfterimages = false;
    float afterimageTimer = 0.0f;
    int   afterimagesLeft = 0;

    // 잔상·파티클 상수
    static const int   AFTERIMAGE_COUNT = 5;
    static const float AFTERIMAGE_INTERVAL; // 잔상 간격 (초)
    static const float AFTERIMAGE_LIFE;     // 잔상 수명 (초)
    static const int   PARTICLE_COUNT = 18;
    static const float PARTICLE_LIFE;      // 파티클 수명 (초)

public:
    DashTrailRenderer()
        : rng(std::random_device{}())
        , randAngle(0.0f, 6.28318f)
        , randSpeed(60.0f, 200.0f)
        , randSize(3.0f, 7.0f)
    {}

    void Start(GraphicsContext*) override {}
    void Input()  override {}

    // PlayerController::TryDash() 에서 호출
    // 잔상 스폰 예약 및 파티클 즉시 생성
    void OnDash(float startX, float startY, float endX, float endY,
        float dirX, float dirY)
    {
        dashDirX = dirX;  dashDirY = dirY;
        dashStartX = startX; dashStartY = startY;
        dashEndX = endX;   dashEndY = endY;

        // 잔상 연속 스폰 예약 (Update 에서 간격마다 1개씩 생성)
        spawningAfterimages = true;
        afterimageTimer = 0.0f;
        afterimagesLeft = AFTERIMAGE_COUNT;

        // 파티클 즉시 생성 : 역방향 편향 + 랜덤 방향 혼합
        for (int i = 0; i < PARTICLE_COUNT; ++i)
        {
            float angle = randAngle(rng);
            float speed = randSpeed(rng);
            // 역방향 60% + 랜덤 40% 혼합
            float vx = cosf(angle) * speed * 0.4f + (-dirX) * speed * 0.6f;
            float vy = sinf(angle) * speed * 0.4f + (-dirY) * speed * 0.6f;

            DashParticle p;
            p.x = startX; p.y = startY;
            p.vx = vx;    p.vy = vy;
            p.alpha = 1.0f;
            p.lifetime = PARTICLE_LIFE;
            p.size = randSize(rng);
            particles.push_back(p);
        }
    }

    void Update(float dt) override
    {
        // 잔상 연속 스폰 : 시작-끝 구간을 선형 보간하여 위치 결정
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

        // 잔상 수명 감소 및 알파 페이드아웃
        for (auto& img : afterimages)
        {
            img.lifetime -= dt;
            img.alpha = img.lifetime / AFTERIMAGE_LIFE * 0.75f;
        }
        afterimages.erase(
            std::remove_if(afterimages.begin(), afterimages.end(),
                [](const DashAfterimage& a) { return a.lifetime <= 0.0f; }),
            afterimages.end());

        // 파티클 이동·감속·수명 감소
        for (auto& p : particles)
        {
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            p.lifetime -= dt;
            p.alpha = p.lifetime / PARTICLE_LIFE;
            // 매 프레임 속도의 3배 비율로 감속
            p.vx *= (1.0f - dt * 3.0f);
            p.vy *= (1.0f - dt * 3.0f);
        }
        particles.erase(
            std::remove_if(particles.begin(), particles.end(),
                [](const DashParticle& p) { return p.lifetime <= 0.0f; }),
            particles.end());
    }

    // PrimitiveBatch 에 잔상·파티클 도형 누적
    void Render(GraphicsContext* gfx) override
    {
        if (gState != GameState::PLAYING) return;

        // 잔상 : 함선 실루엣 삼각형 3개 (몸체 + 좌/우 날개)
        for (auto& img : afterimages)
        {
            float a = img.alpha;
            float hw = 12.0f, hh = 12.0f;
            XMFLOAT4 col = { 0.3f * a, 0.8f * a, a, a }; // 하늘색 계열

            // 몸체 삼각형
            gfx->Batch.AddTri(
                { img.x,              img.y - hh * 2.0f },
                { img.x + hw,         img.y + hh },
                { img.x - hw,         img.y + hh }, col);
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

        // 파티클 : 작은 사각형 (흰색, 알파 페이드아웃)
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
// PlayerController
// 방향키 입력으로 플레이어를 이동시키고
// SPACE 로 대시를 발동하는 컴포넌트
// ============================================================
class PlayerController : public Component
{
    XMFLOAT2 moveDir = { 0, 0 }; // 현재 프레임 입력 방향 (정규화 전)
    XMFLOAT2 lastDir = { 1, 0 }; // 마지막 이동 방향 (정지 상태 대시에 사용)

public:
    DashTrailRenderer* pTrail = nullptr; // 대시 이펙트 컴포넌트 참조

    void Start(GraphicsContext*) override
    {
        printf("[Player] Started. 방향키: 이동 | SPACE: 대시 | ESC: 종료\n");
    }

    // 방향키 입력을 moveDir 에 누적
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

        // 대시 쿨타임 감소
        if (gDashCooldown > 0.0f) gDashCooldown -= dt;

        // 이동 방향 저장 (정규화) — 정지 시에는 마지막 방향 유지
        float len = sqrtf(moveDir.x * moveDir.x + moveDir.y * moveDir.y);
        if (len > 0.0f) { lastDir.x = moveDir.x / len; lastDir.y = moveDir.y / len; }

        // 일반 이동
        pOwner->posX += moveDir.x * PLAYER_SPEED * dt;
        pOwner->posY += moveDir.y * PLAYER_SPEED * dt;

        // 화면 경계 클램프
        if (pOwner->posX < PLAYER_HALF_W)             pOwner->posX = PLAYER_HALF_W;
        if (pOwner->posX > SCREEN_W - PLAYER_HALF_W)  pOwner->posX = SCREEN_W - PLAYER_HALF_W;
        if (pOwner->posY < PLAYER_HALF_H)             pOwner->posY = PLAYER_HALF_H;
        if (pOwner->posY > SCREEN_H - PLAYER_HALF_H)  pOwner->posY = SCREEN_H - PLAYER_HALF_H;

        // 대시 발동 : SPACE 엣지 입력 + 쿨타임 완료
        if ((GetAsyncKeyState(VK_SPACE) & 0x0001) && gDashCooldown <= 0.0f)
            TryDash();
    }

    // 대시 실행
    // lastDir 을 4방향으로 스냅 → DASH_DISTANCE 만큼 순간이동 → 쿨타임 설정
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
// BlinkRenderer
// 무적 상태 중 ColorMaterial 의 색상을 0.1초 주기로 토글하여
// 플레이어가 깜빡이는 시각 효과를 연출
// 무적 해제 시 원래 하늘색으로 복원
// ============================================================
class BlinkRenderer : public Component
{
    ColorMaterial* pMat = nullptr;
    float          blinkTimer = 0.0f;
    bool           visible = true;
    std::mt19937   rng;
    std::uniform_real_distribution<float> randCol; // 보일 때 랜덤 색상 범위

    static const float BLINK_INTERVAL; // 깜빡임 주기 (초)

public:
    BlinkRenderer(ColorMaterial* mat)
        : pMat(mat)
        , rng(std::random_device{}())
        , randCol(0.3f, 1.0f) // 너무 어둡지 않도록 0.3~1.0
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
                    // 보일 때마다 랜덤 색상으로 변경 (무지개 효과)
                    pMat->SetColor({ randCol(rng), randCol(rng), randCol(rng), 1.0f });
                else
                    // 안 보일 때 완전 투명
                    pMat->SetColor({ 0.0f, 0.0f, 0.0f, 0.0f });
            }
        }
        else
        {
            // 무적 해제 시 원래 하늘색으로 복원
            visible = true;
            blinkTimer = 0.0f;
            pMat->SetColor({ 0.3f, 0.8f, 1.0f, 1.0f });
        }
    }

    void Render(GraphicsContext*) override {}
};
const float BlinkRenderer::BLINK_INTERVAL = 0.1f;

// ============================================================
// ObstacleController
// 장애물의 속도(velX/Y)에 따라 매 프레임 위치를 이동하고
// 화면 밖으로 나가면 active = false 로 오브젝트 풀에 반환
// ============================================================
class ObstacleController : public Component
{
public:
    float velX = 0.0f; // X 이동 속도 (픽셀/초)
    float velY = 0.0f; // Y 이동 속도 (픽셀/초)

    void Start(GraphicsContext*) override {}
    void Input()  override {}

    void Update(float dt) override
    {
        if (!pOwner->active) return;

        pOwner->posX += velX * dt;
        pOwner->posY += velY * dt;

        // 화면 밖으로 나가면 비활성화 (오브젝트 풀 반환)
        if (pOwner->posY > SCREEN_H + OBS_HALF_H * 2) pOwner->active = false;
        if (pOwner->posY < -OBS_HALF_H * 2)             pOwner->active = false;
        if (pOwner->posX > SCREEN_W + OBS_HALF_W * 2) pOwner->active = false;
        if (pOwner->posX < -OBS_HALF_W * 2)             pOwner->active = false;
    }

    void Render(GraphicsContext*) override {}
};

// ============================================================
// CooldownBarRenderer
// 대시 쿨타임 게이지를 PrimitiveBatch 로 DirectX 렌더링
// 배경(회색) → 채워진 부분(하늘색/청색) → 외곽선(흰색 라인) 순서로 그림
// 텍스트 레이블("DASH READY" / "DASH Xs")은 TextRenderer::RenderUI 에서 처리
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

        // 충전 비율 계산 (0.0 = 비어있음, 1.0 = 완전 충전)
        float ratio = (gDashCooldown <= 0.0f) ? 1.0f
            : 1.0f - (gDashCooldown / DASH_COOLDOWN);
        int fillW = (int)(GAUGE_W * ratio);

        // 완전 충전 : 하늘색 / 충전 중 : 어두운 청색
        XMFLOAT4 gaugeCol = (gDashCooldown <= 0.0f)
            ? XMFLOAT4{ 0.39f, 0.86f, 1.0f, 1.0f }
        : XMFLOAT4{ 0.16f, 0.39f, 0.71f, 1.0f };

        if (fillW > 0)
            gfx->Batch.AddRect(
                (float)GAUGE_X, (float)GAUGE_Y,
                (float)(GAUGE_X + fillW), (float)(GAUGE_Y + GAUGE_H),
                gaugeCol);

        // 외곽선 (4개 선분으로 사각형 테두리)
        XMFLOAT4 outlineCol = { 0.7f, 0.7f, 0.7f, 1.0f };
        float x0 = (float)GAUGE_X, y0 = (float)GAUGE_Y;
        float x1 = (float)(GAUGE_X + GAUGE_W), y1 = (float)(GAUGE_Y + GAUGE_H);
        gfx->Batch.AddLine({ x0, y0 }, { x1, y0 }, outlineCol); // 상단
        gfx->Batch.AddLine({ x1, y0 }, { x1, y1 }, outlineCol); // 우측
        gfx->Batch.AddLine({ x1, y1 }, { x0, y1 }, outlineCol); // 하단
        gfx->Batch.AddLine({ x0, y1 }, { x0, y0 }, outlineCol); // 좌측
    }
};

// ============================================================
// ShieldRenderer
// 무적 상태 중 플레이어 주위에 회전하는 육각형 쉴드를 렌더링
// PrimitiveBatch LINELIST 사용
// 내·외 두 겹 라인 + 꼭짓점 강조 사각형으로 구성
// 시간 경과에 따라 색상이 노란색 → 빨간색으로 변함
// ============================================================
class ShieldRenderer : public Component
{
    float rotAngle = 0.0f; // 현재 회전 각도 (라디안, 누적)
    float pulseTimer = 0.0f; // 크기 진동 타이머

    enum { SIDES = 6 }; // 육각형

    static const float SHIELD_RADIUS; // 기본 반지름 (픽셀)
    static const float ROTATE_SPEED;  // 회전 속도 (라디안/초)
    static const float PULSE_SPEED;   // 크기 진동 속도

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

        // 무적 잔여 비율로 색상 결정 (1.0=노란색, 0.0=빨간색)
        float ratio = gInvTimer / INVINCIBLE_DURATION;
        float pulse = sinf(pulseTimer) * 6.0f; // ±6px 크기 진동
        float r = SHIELD_RADIUS + pulse;

        float R = 1.0f;
        float G = 0.2f + 0.78f * ratio;
        float B = 0.2f * ratio;

        static const float TWO_PI = 6.28318530718f;

        // 두 겹 라인 (바깥쪽은 절반 밝기)
        for (int layer = 0; layer < 2; ++layer)
        {
            float lr = r + layer * 4.0f;                          // 레이어별 반지름
            float dim = (layer == 0) ? 1.0f : 0.5f;               // 바깥쪽 어둡게
            XMFLOAT4 col = { R * dim, G * dim, B * dim, 1.0f };

            std::vector<XMFLOAT2> pts(SIDES);
            for (int i = 0; i < SIDES; ++i)
            {
                float angle = rotAngle + (i * TWO_PI / SIDES);
                pts[i] = { pOwner->posX + cosf(angle) * lr,
                           pOwner->posY + sinf(angle) * lr };
            }
            gfx->Batch.AddPolyline(pts, col, true); // 닫힌 다각형
        }

        // 꼭짓점 강조 : 작은 흰색 사각형
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
// 비트맵 폰트 텍스처 상수
// 텍스처 한 장에 모든 문자를 격자 형태로 배치
// 셀 크기 : 32×48px  /  한 줄 16칸  /  3줄
// 텍스처 전체 크기 : 512×144px
//
// 문자 배치:
//   행 0 (인덱스 0~15 ) : 0123456789ABCDEF
//   행 1 (인덱스 16~31) : GHIJKLMNOPQRSTUV
//   행 2 (인덱스 32~41) : WXYZ+:*.! (공백)
// ============================================================
static const int FONT_CELL_W = 32;
static const int FONT_CELL_H = 48;
static const int FONT_COLS = 16;
static const int FONT_ROWS = 3;
static const int FONT_TEX_W = FONT_CELL_W * FONT_COLS; // 512
static const int FONT_TEX_H = FONT_CELL_H * FONT_ROWS; // 144

// ============================================================
// TextRenderer
// [별 배경] : Star 구조체 200개를 PrimitiveBatch 사각형으로 렌더링
//             매 프레임 vy 속도로 아래로 이동, 화면 밖 나가면 위에서 재등장
// [비트맵 폰트] :
//   Initialize() 1회 — GDI 메모리 DC 에 문자 그리기 → D3D11 텍스처 업로드
//                       → GDI 즉시 해제 (이후 GDI 완전 미사용)
//   RenderUI()   매 프레임 — DrawString() 으로 UV 쿼드 누적 → _FlushText()
// ============================================================
class TextRenderer
{
    // ── DirectX 텍스트 렌더링 리소스 ────────────────────────
    ID3D11ShaderResourceView* fontSRV = nullptr; // 비트맵 폰트 텍스처 SRV
    ID3D11SamplerState* sampler = nullptr; // 선형 샘플러
    ID3D11Buffer* identityCB = nullptr; // b0: 항등 행렬
    ID3D11VertexShader* texVS = nullptr; // UV 지원 버텍스 셰이더
    ID3D11PixelShader* texPS = nullptr; // 텍스처 샘플링 픽셀 셰이더
    ID3D11InputLayout* texLayout = nullptr; // POSITION+TEXCOORD+COLOR 레이아웃
    ID3D11Buffer* texVB = nullptr; // 텍스트 쿼드용 Dynamic 버텍스 버퍼

    static const UINT TEX_VB_MAX = 6 * 1024; // 최대 1024 글자 (글자당 6 버텍스)

    // 텍스처 셰이더용 버텍스 (위치 + UV + 색상)
    struct TexVertex
    {
        XMFLOAT3 pos;
        XMFLOAT2 uv;
        XMFLOAT4 col;
    };
    std::vector<TexVertex> texVerts; // 프레임당 쿼드 누적 배열

    // UV 지원 셰이더 소스 (TextRenderer 전용)
    static const std::string TEX_SHADER_SRC;

    // ── 별 배경 ─────────────────────────────────────────────
    struct Star
    {
        float x, y;         // 현재 위치 (픽셀)
        float brightness;   // 밝기 (0.4~1.0)
        float vy;           // 낙하 속도 (픽셀/초, 아래 방향 +)
    };
    std::vector<Star> stars;

    GraphicsContext* gfx = nullptr;

public:
    // ── 문자 → 텍스처 셀 인덱스 변환 ───────────────────────
    // 텍스처 배치 순서에 따라 인덱스를 반환
    static int CharToIndex(char c)
    {
        c = (char)toupper((unsigned char)c);
        if (c >= '0' && c <= '9') return c - '0';           // 0~9  : 인덱스 0~9
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');    // A~F  : 인덱스 10~15
        if (c >= 'G' && c <= 'V') return 16 + (c - 'G');    // G~V  : 인덱스 16~31
        if (c >= 'W' && c <= 'Z') return 32 + (c - 'W');    // W~Z  : 인덱스 32~35
        if (c == '+') return 36;
        if (c == ':') return 37;
        if (c == '*') return 38;
        if (c == '.') return 39;
        if (c == '!') return 40;
        return 41; // 공백 및 미정의 문자
    }

    // ── 초기화 ───────────────────────────────────────────────
    void Initialize(HWND hwnd, GraphicsContext* gfxPtr)
    {
        gfx = gfxPtr;

        // 1. 별 200개 생성 (랜덤 위치·밝기·낙하 속도)
        std::mt19937 rng(12345); // 고정 시드로 매 실행 동일한 별 배치
        std::uniform_real_distribution<float> rX(0.f, (float)SCREEN_W);
        std::uniform_real_distribution<float> rY(0.f, (float)SCREEN_H);
        std::uniform_real_distribution<float> rB(0.4f, 1.0f);
        std::uniform_real_distribution<float> rV(20.0f, 60.0f); // 낙하 속도 범위
        for (int i = 0; i < 200; ++i)
            stars.push_back({ rX(rng), rY(rng), rB(rng), rV(rng) });

        // 2. 텍스처 전용 셰이더 컴파일
        _CompileTexShader();

        // 3. 폰트 텍스처 생성 (GDI → D3D11, 단 1회)
        _BuildFontTexture(hwnd);

        // 4. 선형 샘플러 생성
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        gfx->Device->CreateSamplerState(&sd, &sampler);

        // 5. 텍스트 쿼드용 Dynamic 버텍스 버퍼 생성
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(TexVertex) * TEX_VB_MAX;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        gfx->Device->CreateBuffer(&bd, nullptr, &texVB);

        // 6. b0 항등 행렬 상수 버퍼 생성 (NDC 좌표 직접 전달)
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
    }

    // ── 별 배경 스크롤 업데이트 (GameLoop::Update 에서 호출) ──
    void Update(float dt)
    {
        for (auto& s : stars)
        {
            s.y += s.vy * dt;        // 아래로 이동 (우주선이 위로 나는 시각 효과)
            if (s.y > SCREEN_H + 2)  // 화면 하단을 벗어나면
                s.y = -2.0f;         // 화면 상단에서 재등장
        }
    }

    // ── 별 배경 렌더링 (PrimitiveBatch 에 2×2 사각형 누적) ──
    void RenderStars()
    {
        for (auto& s : stars)
        {
            XMFLOAT4 col = { s.brightness, s.brightness, s.brightness, 1.0f };
            gfx->Batch.AddRect(s.x - 1, s.y - 1, s.x + 1, s.y + 1, col);
        }
    }

    // ── 문자열을 TexVertex 쿼드로 누적 ──────────────────────
    // x, y  : 픽셀 좌상단 시작 위치
    // charW : 글자 출력 너비 (픽셀)
    // charH : 글자 출력 높이 (픽셀)
    // col   : 색상 (기본 흰색)
    void DrawString(const std::string& text, float x, float y,
        float charW, float charH, XMFLOAT4 col = { 1,1,1,1 })
    {
        float cx = x; // 현재 글자 출력 X 위치
        for (char c : text)
        {
            // 공백 처리 : 글자 너비의 60% 만큼 이동
            if (c == ' ') { cx += charW * 0.6f; continue; }

            // 문자 → 텍스처 셀 위치 계산
            int idx = CharToIndex(c);
            int cellX = idx % FONT_COLS;
            int cellY = idx / FONT_COLS;

            // UV 좌표 계산 (0~1 정규화)
            float u0 = (cellX * FONT_CELL_W) / (float)FONT_TEX_W;
            float v0 = (cellY * FONT_CELL_H) / (float)FONT_TEX_H;
            float u1 = ((cellX + 1) * FONT_CELL_W) / (float)FONT_TEX_W;
            float v1 = ((cellY + 1) * FONT_CELL_H) / (float)FONT_TEX_H;

            // 픽셀 좌표 → NDC 변환
            float x0 = PxToNdcX(cx), y0 = PxToNdcY(y);
            float x1 = PxToNdcX(cx + charW), y1 = PxToNdcY(y + charH);

            // 사각형 2삼각형 (6 버텍스)
            texVerts.push_back({ { x0, y0, 0 }, { u0, v0 }, col });
            texVerts.push_back({ { x1, y0, 0 }, { u1, v0 }, col });
            texVerts.push_back({ { x1, y1, 0 }, { u1, v1 }, col });
            texVerts.push_back({ { x0, y0, 0 }, { u0, v0 }, col });
            texVerts.push_back({ { x1, y1, 0 }, { u1, v1 }, col });
            texVerts.push_back({ { x0, y1, 0 }, { u0, v1 }, col });

            cx += charW; // 다음 글자 위치
        }
    }

    // ── UI 텍스트 렌더링 (Present 전에 호출) ─────────────────
    // 화면 상태별로 DrawString 호출 후 _FlushText 로 GPU 제출
    void RenderUI()
    {
        if (gState == GameState::MAIN)
        {
            // 타이틀
            DrawString("DODGE++", 420, 200, 48, 64, { 1.0f, 1.0f, 1.0f, 1.0f });
            // 최고 점수
            DrawString("BEST : " + std::to_string((int)gHighScore),
                490, 290, 22, 32, { 0.4f, 1.0f, 0.7f, 1.0f });
            // 조작 안내
            DrawString("SPACE  : START", 450, 350, 20, 28, { 0.8f, 0.8f, 0.8f, 1.0f });
            DrawString("ARROW  : MOVE", 450, 390, 20, 28, { 0.8f, 0.8f, 0.8f, 1.0f });
            DrawString("SPACE  : DASH 3S COOL", 450, 430, 20, 28, { 0.4f, 0.86f,1.0f, 1.0f });
            DrawString("STAR   : INVINCIBLE", 450, 470, 20, 28, { 1.0f, 0.9f, 0.2f, 1.0f });
            DrawString("ESC    : QUIT", 450, 510, 20, 28, { 0.8f, 0.8f, 0.8f, 1.0f });
        }
        else if (gState == GameState::PLAYING)
        {
            // 현재 점수
            DrawString("SCORE : " + std::to_string((int)gScore),
                10, 10, 20, 28, { 1.0f, 1.0f, 0.4f, 1.0f });
            // 무적 상태 표시
            if (gIsInvincible)
                DrawString("INVINCIBLE " + std::to_string((int)(gInvTimer + 1.0f)) + "S",
                    SCREEN_W / 2 - 110, 10, 20, 28, { 1.0f, 0.86f, 0.2f, 1.0f });

            // 대시 쿨타임 레이블 (게이지 위)
            std::string label = (gDashCooldown <= 0.0f)
                ? "DASH READY"
                : ("DASH " + std::to_string((int)(gDashCooldown + 1.0f)) + "S");
            DrawString(label, (float)GAUGE_X, (float)(GAUGE_Y - 30), 18, 24,
                { 0.86f, 0.86f, 0.86f, 1.0f });
        }
        else if (gState == GameState::GAMEOVER)
        {
            DrawString("GAME OVER", 400, 240, 44, 60, { 1.0f, 0.3f, 0.3f, 1.0f });
            DrawString("SCORE : " + std::to_string((int)gScore),
                490, 330, 22, 32, { 1.0f, 1.0f, 0.4f, 1.0f });
            DrawString("BEST  : " + std::to_string((int)gHighScore),
                490, 370, 22, 32, { 0.4f, 1.0f, 0.7f, 1.0f });
            DrawString("R   : RESTART", 490, 420, 20, 28, { 0.8f, 0.8f, 0.8f, 1.0f });
            DrawString("ESC : QUIT", 490, 460, 20, 28, { 0.8f, 0.8f, 0.8f, 1.0f });
            DrawString("M   : MENU", 490, 500, 20, 28, { 0.7f, 0.86f, 1.0f, 1.0f });
        }

        // 누적된 쿼드를 GPU 에 일괄 제출
        _FlushText();
    }

    ~TextRenderer()
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
    // ── 폰트 텍스처 빌드 ─────────────────────────────────────
    // 단 1회 호출 (Initialize 내부)
    // GDI 메모리 DC → 문자 비트맵 → RGBA 변환 → D3D11 텍스처 → GDI 해제
    void _BuildFontTexture(HWND hwnd)
    {
        const int W = FONT_TEX_W; // 512
        const int H = FONT_TEX_H; // 144

        // GDI 메모리 DC 및 DIB 섹션 생성
        HDC screenDC = GetDC(hwnd);
        HDC memDC = CreateCompatibleDC(screenDC);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = W;
        bmi.bmiHeader.biHeight = -H; // 음수 = 상단 기준(top-down)
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);

        // 배경 초기화 (검정, 알파=0)
        memset(bits, 0, W * H * 4);

        // 폰트 설정 (Consolas Bold, 셀 높이 - 6px 패딩)
        HFONT hFont = CreateFont(
            FONT_CELL_H - 6, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        SelectObject(memDC, hFont);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 255)); // 흰색으로 그려야 알파 계산 정확

        // 문자 목록 (행 순서대로 배치)
        const char* CHARS =
            "0123456789ABCDEF"  // 행 0
            "GHIJKLMNOPQRSTUV"  // 행 1
            "WXYZ+:*.! ";       // 행 2 (마지막 공백은 빈 슬롯)

        // 각 문자를 셀 위치에 렌더링
        for (int i = 0; CHARS[i] != '\0'; ++i)
        {
            int col = i % FONT_COLS;
            int row = i / FONT_COLS;
            int px = col * FONT_CELL_W + 2; // 좌우 2px 패딩
            int py = row * FONT_CELL_H + 2; // 상하 2px 패딩

            wchar_t wc[2] = { (wchar_t)(unsigned char)CHARS[i], 0 };
            TextOutW(memDC, px, py, wc, 1);
        }

        // GDI 비트맵(BGR0) → RGBA 변환
        // 픽셀 밝기를 알파값으로 사용하여 안티에일리어싱 보존
        std::vector<uint32_t> rgba(W * H);
        BYTE* src = (BYTE*)bits;
        for (int i = 0; i < W * H; ++i)
        {
            BYTE b = src[i * 4 + 0];
            BYTE g = src[i * 4 + 1];
            BYTE r = src[i * 4 + 2];
            BYTE a = (BYTE)(((int)r + g + b) / 3); // 밝기 평균 → 알파
            rgba[i] = ((uint32_t)a << 24) | ((uint32_t)b << 16)
                | ((uint32_t)g << 8) | (uint32_t)r;
        }

        // D3D11 텍스처 생성 및 SRV 획득
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = W;
        td.Height = H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA tsd = {};
        tsd.pSysMem = rgba.data();
        tsd.SysMemPitch = W * 4;

        ID3D11Texture2D* tex = nullptr;
        gfx->Device->CreateTexture2D(&td, &tsd, &tex);
        gfx->Device->CreateShaderResourceView(tex, nullptr, &fontSRV);
        tex->Release(); // SRV 가 참조를 유지하므로 텍스처 즉시 해제 가능

        // GDI 리소스 즉시 해제 — 이후 절대 GDI 사용 없음
        SelectObject(memDC, oldBmp);
        DeleteObject(hFont);
        DeleteObject(hBmp);
        DeleteDC(memDC);
        ReleaseDC(hwnd, screenDC);
    }

    // ── 텍스처 전용 셰이더 컴파일 ───────────────────────────
    // 메인 셰이더와 별도로 TEXCOORD 시멘틱을 가진 입력 레이아웃 필요
    void _CompileTexShader()
    {
        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errBlob = nullptr;

        D3DCompile(TEX_SHADER_SRC.c_str(), TEX_SHADER_SRC.size(),
            nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }

        D3DCompile(TEX_SHADER_SRC.c_str(), TEX_SHADER_SRC.size(),
            nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, &errBlob);
        if (errBlob) { OutputDebugStringA((char*)errBlob->GetBufferPointer()); errBlob->Release(); }

        if (!vsBlob || !psBlob) return;

        gfx->Device->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &texVS);
        gfx->Device->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &texPS);

        // POSITION(3f) + TEXCOORD(2f) + COLOR(4f) 입력 레이아웃
        D3D11_INPUT_ELEMENT_DESC ied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        gfx->Device->CreateInputLayout(
            ied, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &texLayout);

        vsBlob->Release();
        psBlob->Release();
    }

    // ── 누적된 텍스트 쿼드 GPU 제출 ─────────────────────────
    void _FlushText()
    {
        if (texVerts.empty() || !texVB || !fontSRV) return;

        auto* ctx = gfx->ImmediateContext;

        // 텍스처 셰이더·레이아웃·리소스 바인딩
        ctx->IASetInputLayout(texLayout);
        ctx->VSSetShader(texVS, nullptr, 0);
        ctx->PSSetShader(texPS, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &identityCB);   // b0: 항등 행렬
        ctx->PSSetShaderResources(0, 1, &fontSRV);       // t0: 폰트 텍스처
        ctx->PSSetSamplers(0, 1, &sampler);              // s0: 선형 샘플러

        // Dynamic 버퍼에 데이터 업로드
        D3D11_MAPPED_SUBRESOURCE ms = {};
        if (SUCCEEDED(ctx->Map(texVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        {
            size_t copyCount = min(texVerts.size(), (size_t)TEX_VB_MAX);
            memcpy(ms.pData, texVerts.data(), sizeof(TexVertex) * copyCount);
            ctx->Unmap(texVB, 0);
        }

        UINT stride = sizeof(TexVertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &texVB, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->Draw((UINT)min(texVerts.size(), (size_t)TEX_VB_MAX), 0);

        texVerts.clear();

        // 토폴로지 복원 (다음 패스를 위해)
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
};

// ── 텍스처 전용 셰이더 소스 ─────────────────────────────────
// b0 : 항등 행렬 (NDC 좌표 직접 사용)
// t0 : 비트맵 폰트 텍스처
// s0 : 선형 샘플러
// PS : 텍스처 알파 × 지정 색상 → 반투명 텍스트 렌더링
const std::string TextRenderer::TEX_SHADER_SRC = R"(
    cbuffer cbWorld : register(b0) { matrix matWorld; }

    Texture2D    gFont   : register(t0);
    SamplerState gSampler: register(s0);

    struct VS_IN { float3 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
    struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };

    PS_IN VS(VS_IN input)
    {
        PS_IN o;
        // pos 는 이미 NDC — matWorld 는 항등 행렬이므로 변환 없이 통과
        o.pos = mul(float4(input.pos, 1.0f), matWorld);
        o.uv  = input.uv;
        o.col = input.col;
        return o;
    }

    float4 PS(PS_IN input) : SV_Target
    {
        // 텍스처에서 폰트 알파 샘플링 → 지정 색상과 곱하여 출력
        float4 tex = gFont.Sample(gSampler, input.uv);
        return float4(input.col.rgb, input.col.a * tex.a);
    }
)";

// ============================================================
// GameManager
// 게임 진행 전반을 관리
//   - 장애물 스폰 (오브젝트 풀 방식, 난이도 단계별 속도·빈도 증가)
//   - 무적 아이템 스폰 (15초마다 랜덤 위치)
//   - 무적 상태 타이머 관리
//   - 아이템 획득 및 충돌 판정
//   - 난이도 단계 상승 (20초마다, 최대 6단계)
// ============================================================
class GameManager
{
    // 랜덤 생성기 및 분포
    std::mt19937                          rng;
    std::uniform_real_distribution<float> randPosX;  // 아이템 스폰 X 범위
    std::uniform_real_distribution<float> randPosY;  // 아이템 스폰 Y 범위
    std::uniform_real_distribution<float> randSpeed; // 장애물 속도 범위
    std::uniform_int_distribution<int>    randSide;  // 스폰 방향 (0~3)
    std::uniform_real_distribution<float> randDiag;  // 대각 방향 비율

    float spawnTimer = 0.0f; // 장애물 스폰 누적 시간
    float itemSpawnTimer = 0.0f; // 아이템 스폰 누적 시간
    int   difficultyLevel = 0;    // 현재 난이도 단계 (0~6)
    float difficultyTimer = 0.0f; // 난이도 상승 누적 시간

public:
    // 외부에서 주입받는 참조 (소유권 없음)
    std::vector<GameObject*>* obstacles = nullptr;
    std::vector<GameObject*>* items = nullptr;
    GameObject* player = nullptr;
    GraphicsContext* gfx = nullptr;

    // 장애물·아이템 Mesh/Material (WinMain 소유, 빌려 씀)
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

    // 현재 난이도에 맞는 속도 범위로 장애물 스폰 위치·속도 계산
    // 4방향 중 랜덤 선택 후 화면 바깥에서 안쪽 방향으로 진행
    void CalcObsSpawn(float& outX, float& outY, float& outVX, float& outVY)
    {
        float sMin, sMax;
        GetSpeedRange(sMin, sMax);
        std::uniform_real_distribution<float> dynSpeed(sMin, sMax);

        int   side = randSide(rng);
        float speed = dynSpeed(rng);
        float diag = randDiag(rng); // 주 방향에 대한 대각 비율

        switch (side)
        {
        case 0: // 위에서 아래로
            outX = randPosX(rng); outY = -OBS_HALF_H;
            outVX = speed * diag; outVY = speed;  break;
        case 1: // 아래에서 위로
            outX = randPosX(rng); outY = SCREEN_H + OBS_HALF_H;
            outVX = speed * diag; outVY = -speed; break;
        case 2: // 왼쪽에서 오른쪽으로
            outX = -OBS_HALF_W;   outY = randPosY(rng);
            outVX = speed;        outVY = speed * diag; break;
        default: // 오른쪽에서 왼쪽으로
            outX = SCREEN_W + OBS_HALF_W; outY = randPosY(rng);
            outVX = -speed;               outVY = speed * diag; break;
        }
    }

    // 게임 초기화 : 전역 상태 및 오브젝트 풀 리셋
    void ResetGame()
    {
        gScore = 0.0f;
        spawnTimer = itemSpawnTimer = 0.0f;
        gIsInvincible = false;
        gInvTimer = gDashCooldown = 0.0f;
        difficultyLevel = 0;
        difficultyTimer = 0.0f;

        // 게임 시작 직전 SPACE 잔여 입력 소비 → 대시 즉시 발동 버그 방지
        GetAsyncKeyState(VK_SPACE);

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

    // 장애물 count 개 스폰 (오브젝트 풀 재사용 우선, 한도 초과 시 신규 생성)
    void SpawnObstacles(int count)
    {
        for (int n = 0; n < count; ++n)
        {
            bool reused = false;
            // 비활성 오브젝트 재사용
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

    // 무적 아이템 스폰 (화면에 동시에 1개만 존재)
    void SpawnInvincibleItem()
    {
        if (!items || !gfx || !itemMesh || !itemMat) return;

        // 이미 활성 아이템이 있으면 스킵
        for (auto* it : *items) if (it->active) return;

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

    // 무적 타이머 감소 및 만료 처리
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

    // 현재 난이도 단계에 따른 장애물 스폰 간격 반환
    // 단계 0: 1.2초 → 단계 6: ~0.4초 (선형 감소)
    float GetSpawnInterval() const
    {
        float t = (float)difficultyLevel / DIFFICULTY_MAX;
        return OBS_SPAWN_INTERVAL * (1.0f - t * 0.67f);
    }

    // 현재 난이도에 따른 장애물 속도 범위 반환
    // 단계마다 최소 +40, 최대 +60 px/초 증가
    void GetSpeedRange(float& outMin, float& outMax) const
    {
        outMin = OBS_SPEED_MIN + difficultyLevel * 40.0f;
        outMax = OBS_SPEED_MAX + difficultyLevel * 60.0f;
    }

    // 현재 난이도에 따른 동시 스폰 개수 (기본 4 + 단계)
    int GetSpawnCount() const { return 4 + difficultyLevel; }

    void Update(float dt)
    {
        if (gState != GameState::PLAYING) return;

        // 생존 시간 누적 (점수)
        gScore += dt;

        // 난이도 단계 상승 (20초마다)
        difficultyTimer += dt;
        if (difficultyTimer >= DIFFICULTY_INTERVAL && difficultyLevel < DIFFICULTY_MAX)
        {
            difficultyLevel++;
            difficultyTimer = 0.0f;
            printf("[Difficulty] 단계 %d 돌입! 동시스폰 %d개\n",
                difficultyLevel, GetSpawnCount());
        }

        // 장애물 스폰
        spawnTimer += dt;
        if (spawnTimer >= GetSpawnInterval())
        {
            SpawnObstacles(GetSpawnCount());
            spawnTimer = 0.0f;
        }

        // 무적 아이템 스폰 (15초마다)
        itemSpawnTimer += dt;
        if (itemSpawnTimer >= ITEM_SPAWN_INTERVAL)
        {
            SpawnInvincibleItem();
            itemSpawnTimer = 0.0f;
        }

        // 무적 타이머 처리
        UpdateInvincible(dt);

        // 아이템 획득 판정 (플레이어 ↔ 아이템 AABB)
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

        // 충돌 판정 (무적 중 제외)
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
// 메인 루프 : Initialize → Run (Input → Update → Render) 반복
// world      : 플레이어 등 항상 활성인 오브젝트
// obstacles  : 장애물 오브젝트 풀
// items      : 무적 아이템 오브젝트 풀
// ============================================================
class GameLoop
{
public:
    WindowContext            win;
    GraphicsContext          gfx;
    DeltaTime                timer;
    std::vector<GameObject*> world;     // 플레이어 등 영속 오브젝트
    std::vector<GameObject*> obstacles; // 장애물 오브젝트 풀
    std::vector<GameObject*> items;     // 아이템 오브젝트 풀
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

    // 입력 처리 : ESC 종료 / 화면 전환 / 플레이어 컴포넌트 Input 전달
    void Input()
    {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) isRunning = false;

        // 메인화면 → 게임 시작
        if (gState == GameState::MAIN && (GetAsyncKeyState(VK_SPACE) & 0x0001))
        {
            gState = GameState::PLAYING;
            manager.ResetGame();
        }

        // 게임오버 → 재시작
        if (gState == GameState::GAMEOVER && (GetAsyncKeyState('R') & 0x0001))
        {
            gState = GameState::PLAYING;
            manager.ResetGame();
        }

        // 게임오버 → 메인화면
        if (gState == GameState::GAMEOVER && (GetAsyncKeyState('M') & 0x0001))
        {
            gState = GameState::MAIN;
            manager.ResetGame();
        }

        for (auto* obj : world) if (obj && obj->active) obj->Input();
    }

    // 업데이트 : GameManager → 별 배경 → 모든 오브젝트 Update
    void Update()
    {
        float dt = timer.GetDelta();

        manager.Update(dt);      // 스폰·충돌·난이도
        text.Update(dt);         // 별 배경 스크롤

        for (auto* obj : world)     if (obj && obj->active) obj->Update(dt, &gfx);
        for (auto* obs : obstacles) if (obs && obs->active) obs->Update(dt, &gfx);
        for (auto* it : items)     if (it && it->active)  it->Update(dt, &gfx);
    }

    // 렌더링 파이프라인 (매 프레임)
    void Render()
    {
        // 1. 화면 초기화 (우주 배경색 : 거의 검정에 가까운 남색)
        float col[] = { 0.0f, 0.0f, 0.05f, 1.0f };
        gfx.ImmediateContext->ClearRenderTargetView(gfx.RTV, col);

        // 뷰포트·렌더타겟 설정
        D3D11_VIEWPORT vp = { 0, 0, (float)SCREEN_W, (float)SCREEN_H, 0, 1 };
        gfx.ImmediateContext->RSSetViewports(1, &vp);
        gfx.ImmediateContext->OMSetRenderTargets(1, &gfx.RTV, NULL);
        gfx.ImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // 알파 블렌딩 설정 (소스 알파 × 소스 + (1 - 소스 알파) × 목적지)
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
        blendState->Release(); // 설정 후 즉시 해제 (State 는 파이프라인에 유지)

        // 2. 별 배경 (PrimitiveBatch 에 사각형 누적)
        text.RenderStars();

        // 3. 정적 Mesh 오브젝트 렌더링 (장애물 → 아이템 → 플레이어 순)
        for (auto* obs : obstacles) if (obs && obs->active) obs->Render(&gfx);
        for (auto* it : items)     if (it && it->active)  it->Render(&gfx);
        for (auto* obj : world)     if (obj && obj->active) obj->Render(&gfx);

        // 4. PrimitiveBatch 일괄 GPU 제출
        //    (별·게이지·쉴드·잔상·파티클 등 AddXxx 로 누적된 것을 한 번에 Draw)
        gfx.Batch.Flush(gfx.ImmediateContext);

        // 5. 비트맵 폰트 UI 렌더링 (점수·타이틀·게임오버 텍스트)
        text.RenderUI();

        // 6. 화면 출력 (GDI 오버레이 없음 — 깜빡임 없음)
        gfx.SwapChain->Present(gfx.VSync, 0);
    }

    // 메시지 루프 : WM_QUIT 또는 isRunning=false 까지 반복
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
// 공통 셰이더 소스 (메인 셰이더)
// 정적 Mesh 오브젝트 및 PrimitiveBatch 가 공유
//
// b0 : matWorld — 오브젝트 위치 이동 행렬 (MeshRenderer) or 항등 행렬 (Batch)
// b1 : tintColor — ColorMaterial 이 단색 설정 / Batch 는 (1,1,1,1) 고정
//
// PS : input.col * tintColor
//   tintColor=(1,1,1,1) 이면 버텍스 col 그대로 출력 (Batch 용)
//   tintColor=단색      이면 단색 오버라이드 (MeshRenderer 용)
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

    float4 PS(PS_IN input) : SV_Target
    {
        return input.col * tintColor;
    }
)";

// ============================================================
// Mesh 생성 헬퍼 함수
// 모두 NDC 단위로 버텍스를 정의하며 SCREEN_W/H 기준으로 정규화
// ============================================================

// 중앙 기준 사각형 (halfW × halfH)
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

// 5각 별 (바깥 꼭짓점 outerR / 안쪽 꼭짓점 innerR, 삼각형 10개)
Mesh* CreateStarMesh(ID3D11Device* device, float outerR, float innerR)
{
    float nOutX = (outerR / SCREEN_W) * 2.0f;
    float nOutY = (outerR / SCREEN_H) * 2.0f;
    float nInX = (innerR / SCREEN_W) * 2.0f;
    float nInY = (innerR / SCREEN_H) * 2.0f;

    XMFLOAT2 pts[10];
    for (int i = 0; i < 10; ++i)
    {
        // XM_PIDIV2 시작 → 꼭짓점이 위쪽을 향함
        float angle = XM_PIDIV2 - (i * XM_2PI / 10.0f);
        if (i % 2 == 0) pts[i] = { cosf(angle) * nOutX, sinf(angle) * nOutY }; // 바깥
        else            pts[i] = { cosf(angle) * nInX,  sinf(angle) * nInY }; // 안쪽
    }

    std::vector<Vertex> verts;
    verts.reserve(30);
    for (int i = 0; i < 10; ++i)
    {
        verts.push_back({ { 0, 0, 0 },                                                    { 1,1,1,1 } });
        verts.push_back({ { pts[i].x, pts[i].y, 0 },                                      { 1,1,1,1 } });
        verts.push_back({ { pts[(i + 1) % 10].x, pts[(i + 1) % 10].y, 0 },               { 1,1,1,1 } });
    }
    Mesh* mesh = new Mesh();
    mesh->Create(device, verts);
    return mesh;
}

// 우주선 (몸체 삼각형 + 좌·우 날개 삼각형, 3개 삼각형 = 9 버텍스)
Mesh* CreateShipMesh(ID3D11Device* device, float r)
{
    float nW = (r / SCREEN_W) * 2.0f;
    float nH = (r / SCREEN_H) * 2.0f;
    std::vector<Vertex> verts;

    // 몸체 (위 꼭짓점 → 우하 → 좌하)
    verts.push_back({ {  0.0f,      nH * 2.0f,  0 }, { 1,1,1,1 } });
    verts.push_back({ {  nW,       -nH,          0 }, { 1,1,1,1 } });
    verts.push_back({ { -nW,       -nH,          0 }, { 1,1,1,1 } });

    // 왼쪽 날개
    verts.push_back({ {  0.0f,     -nH * 0.5f,  0 }, { 1,1,1,1 } });
    verts.push_back({ { -nW,       -nH,          0 }, { 1,1,1,1 } });
    verts.push_back({ { -nW * 2.5f,-nH * 1.5f,  0 }, { 1,1,1,1 } });

    // 오른쪽 날개
    verts.push_back({ {  0.0f,     -nH * 0.5f,  0 }, { 1,1,1,1 } });
    verts.push_back({ {  nW * 2.5f,-nH * 1.5f,  0 }, { 1,1,1,1 } });
    verts.push_back({ {  nW,       -nH,          0 }, { 1,1,1,1 } });

    Mesh* mesh = new Mesh();
    mesh->Create(device, verts);
    return mesh;
}

// 운석 (8각형 팬, 8개 삼각형 = 24 버텍스)
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
        verts.push_back({ { 0, 0, 0 },                                                          { 1,1,1,1 } });
        verts.push_back({ { pts[(i + 1) % POINTS].x, pts[(i + 1) % POINTS].y, 0 },             { 1,1,1,1 } });
        verts.push_back({ { pts[i].x, pts[i].y, 0 },                                            { 1,1,1,1 } });
    }
    Mesh* mesh = new Mesh();
    mesh->Create(device, verts);
    return mesh;
}

// ============================================================
// WndProc
// WM_DESTROY 에서 PostQuitMessage(0) 호출 → 메시지 루프 종료
// ============================================================
LRESULT CALLBACK GlobalWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProc(h, m, w, l);
}

// ============================================================
// WinMain
// 진입점 : GameLoop 생성 → 리소스 초기화 → Run → 리소스 해제
// ============================================================
int WINAPI WinMain(HINSTANCE hI, HINSTANCE, LPSTR, int)
{
    GameLoop gEngine;
    gEngine.Initialize(hI, GlobalWndProc);

    // 버텍스 입력 레이아웃 정의 (POSITION 12바이트 + COLOR 16바이트)
    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    // 공통 셰이더 컴파일 (MeshRenderer + PrimitiveBatch 가 공유)
    ShaderSet shaders = gEngine.gfx.CompileAndCreate(SHADER_SRC, ied, 2);
    if (!shaders.vs || !shaders.ps)
    {
        printf("[Error] Shader compile failed.\n"); return -1;
    }

    // PrimitiveBatch 초기화 (공통 셰이더 포인터 공유)
    if (!gEngine.gfx.Batch.Init(gEngine.gfx.Device, shaders))
    {
        printf("[Error] PrimitiveBatch init failed.\n"); return -1;
    }

    // Mesh 생성 (정적 GPU 버퍼)
    Mesh* playerMesh = CreateShipMesh(gEngine.gfx.Device, 15.0f);    // 우주선
    Mesh* obsMesh = CreateMeteorMesh(gEngine.gfx.Device, 15.0f);  // 운석
    Mesh* itemMesh = CreateStarMesh(gEngine.gfx.Device,            // 무적 아이템 별
        ITEM_HALF_W, ITEM_HALF_W * 0.42f);

    // Material 생성 (공통 셰이더 공유, 색상만 다름)
    ColorMaterial* playerMat = new ColorMaterial(shaders, { 0.3f, 0.8f, 1.0f, 1.0f }, gEngine.gfx.Device); // 하늘색
    ColorMaterial* obsMat = new ColorMaterial(shaders, { 1.0f, 0.5f, 0.2f, 1.0f }, gEngine.gfx.Device); // 주황색
    ColorMaterial* itemMat = new ColorMaterial(shaders, { 1.0f, 0.9f, 0.1f, 1.0f }, gEngine.gfx.Device); // 노란색

    // 플레이어 GameObject 조립
    auto* player = new GameObject(SCREEN_W / 2.0f, SCREEN_H / 2.0f,
        PLAYER_HALF_W, PLAYER_HALF_H);
    auto* trail = new DashTrailRenderer();

    player->AddComponent(new MeshRenderer(playerMesh, playerMat)); // [0] 우주선 렌더링
    player->AddComponent(new PlayerController());                   // [1] 이동·대시 입력
    player->AddComponent(new BlinkRenderer(playerMat));             // [2] 무적 깜빡임
    player->AddComponent(new CooldownBarRenderer());                // [3] 대시 게이지 UI
    player->AddComponent(new ShieldRenderer());                     // [4] 무적 쉴드
    player->AddComponent(trail);                                    // [5] 대시 잔상·파티클

    // PlayerController 에 DashTrailRenderer 연결
    auto* pc = dynamic_cast<PlayerController*>(player->components[1]);
    if (pc) pc->pTrail = trail;

    gEngine.world.push_back(player);

    // GameManager 에 참조 주입
    gEngine.manager.player = player;
    gEngine.manager.obstacles = &gEngine.obstacles;
    gEngine.manager.items = &gEngine.items;
    gEngine.manager.gfx = &gEngine.gfx;
    gEngine.manager.obsMesh = obsMesh;
    gEngine.manager.obsMat = obsMat;
    gEngine.manager.itemMesh = itemMesh;
    gEngine.manager.itemMat = itemMat;

    printf("[Game] 메인화면 | SPACE: 시작 / ESC: 종료\n");

    gEngine.Run(); // 메인 루프 진입

    // 공유 리소스 해제 (GameLoop 소멸자가 먼저 GameObject 들을 해제)
    delete playerMat;
    delete obsMat;
    delete itemMat;
    delete playerMesh;
    delete obsMesh;
    delete itemMesh;
    shaders.Release();

    return 0;
}
