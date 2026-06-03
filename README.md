 Dodge++ - 개발 보고서 및 README

■ 프로젝트 개요
  - 프로젝트명: Dodge++
  - 개발 언어 및 환경: C++ / Windows API / DirectX 11 (Direct3D 11)
  - 주요 특징: 
    * 컴포넌트 기반 아키텍처(Component-Based Architecture) 설계
    * DirectX 11 기반의 고성능 2D/3D 하이브리드 렌더링 파이프라인 구축
    * 오브젝트 풀링(Object Pooling) 시스템을 통한 메모리 및 가비지 컬렉션 최적화
    * 배치 렌더링(Batch Rendering)을 통한 Draw Call 최소화 및 GPU 효율 극대화

■ 개발진 및 역할 분담
  - 조장: 김정일 (12211586) - [A] 게임 핵심 로직, 컴포넌트 시스템, 오브젝트 풀 및 상태 관리 담당
  - 조원: 안시헌 (12211645) - [B] DirectX 11 파이프라인 구축, 셰이더 프로그래밍, 일괄(Batch) 렌더링 시스템 담당


----------------------------------------------------------------------------------------------------------
1. 게임 시스템 및 아키텍처 (Architecture)

■ 주요 객체 구조 (GameObject & Component)
  Dodge++는 컴포넌트 패턴으로 기능을 확장했습니다.
  
  - GameObject: 게임 내 모든 엔티티의 기본 단위로 Transform 및 Component 목록을 보유합니다.
  - 주요 게임 오브젝트 및 컴포넌트 매핑:
    * player: 게임의 주인공 캐릭터
      - Transform, PlayerController (이동, 대시, 보호막 로직)
      - MeshRenderer (플레이어 외형 렌더링)
      - CooldownBarRenderer (스킬 쿨타임 UI)
      - ShieldRenderer (보호막 이펙트 렌더링)
      - DashTrail (대시 잔상 이펙트)
    * gSystemObj: 게임의 흐름과 규칙을 제어하는 관리자 객체
      - GameManagerComponent (장애물/아이템 스폰, 점수 계산, 난이도 조절, 충돌 체크)
    * gUIObj: 배경을 담당하는 그래픽 객체
      - StarBackgroundRenderer (우주 공간의 별 움직임)
    * gUITextObj: 화면에 텍스트 정보를 출력하는 객체
      - UITextRenderer (GDI 텍스처를 이용한 비트맵 폰트 생성 및 출력)

■ 객체 생명주기 및 메모리 관리 (Lifecycle & Object Pool)
  프레임 드랍을 방지하고 게임의 깜빡임이 없이 실시간 성능을 높이기 위해 동적 할당을 최소화하는 구조를 갖추고 있습니다.
  
  - 고정 오브젝트 (player, gSystemObj, gUIObj, gUITextObj):
    * WinMain 진입 시 생성되어 애플리케이션 종료 시 소멸합니다.
    * 첫 번째 Update() 루프가 실행될 때 Start() 초기화가 호출됩니다.
  - 풀링 오브젝트 (obstacle, item):
    * GameManagerComponent 내부에 벡터 풀(Vector Pool) 형태로 관리됩니다.
    * 최초 Spawn 시 풀이 부족하면 새롭게 new로 생성하지만, 화면 밖으로 이탈하면 파괴하지 않고 active=false 상태로 반환됩니다.
    * 재사용 시 active=true로 전환되어 첫 Update()를 다시 밟는 효율적인 생명주기를 가집니다.

----------------------------------------------------------------------------------------------------------
2. 렌더링 파이프라인 및 최적화 (Rendering Pipeline)


Dodge++는 GPU로 전달되는 데이터의 병목을 줄이기 위해 Batching 구조와 DirectX 11 렌더링를 활용합니다.

■ 프레임별 렌더링 순서 (Rendering Sequence)
  1. ClearRenderTargetView
     - 화면을 깨끗하게 지우고 새로운 프레임을 준비합니다.
  2. gUIObj::Render
     - StarBackgroundRenderer를 실행하여 배경 별 데이터를 Batch 버퍼에 누적합니다.
  3. MeshRenderer::Render
     - 장애물(Obstacles), 아이템(Items), 플레이어(Player)의 메시를 준비합니다.
  4. CooldownBar / Shield / DashTrail::Render
     - 플레이어 관련 부가 이펙트 및 스킬 UI 데이터를 Batch 버퍼에 최종 누적합니다.
  5. Batch.Flush
     - 지금까지 누적된 정점 데이터를 모아 `TRIANGLELIST` 또는 `LINELIST` 형태로 GPU에 일괄 제출(Draw Call)합니다.
  6. gUITextObj::Render
     - UITextRenderer가 비트맵 폰를 최상단에 Draw하여 실시간 UI 텍스트(점수, 상태 등)를 오버레이합니다.
  7. SwapChain::Present
     - 백버퍼의 최종 결과물을 전면 버퍼로 전환하여 화면에 출력합니다.

■ 리소스 최적화 및 디바이스 컨텍스트 관리
  - 셰이더 및 컴파일: D3DCompileFromFile을 통해 vertex shader(VS) 및 pixel shader(PS)를 런타임에 빌드합니다.
  - 버퍼 관리: 효율적인 데이터 전송을 위해 상수 버퍼(Constant Buffer)와 정점 버퍼(Vertex Buffer)를 관리합니다.
  - 상태 초기화: 애플리케이션 종료 시 Direct3D 디바이스(ID3D11Device), 컨텍스트(ID3D11DeviceContext), 스왑 체인 및 각종 렌더 타겟 뷰를 `.Release()` 호출을 통해 누수 없이 안전하게 해제합니다.


----------------------------------------------------------------------------------------------------------
3. 컴파일 및 실행 방법 (Compilation & Execution)

■ 시스템 요구 사양
  - OS: Windows 10 / 11
  - 그래픽 라이브러리: DirectX 11 (Windows 10/11 기본 내장)
  - Visual C++ 재배포 패키지: Microsoft Visual C++ 2022 Redistributable (x64)

■ 실행 방법
  - 별도의 설치 과정 없이 Dodge++.exe를 더블클릭하여 즉시 실행할 수 있습니다.
  - 실행 시 디버깅용 콘솔 창이 함께 활성화됩니다. (정상 동작)

■ 조작 방법
  - [SPACE] : 메인 화면 시작 및 게임 진입
  - [방향키] : 플레이어 캐릭터 이동
  - [space] : 대시(Dash) 특수 스킬 발동
