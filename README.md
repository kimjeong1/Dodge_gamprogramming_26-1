Dodge++ - 개발 보고서 및 README

■ 팀 정보
  - 팀명: Two Twenty One
  - 팀원: 12211586 (김정일), 12211645 (안시헌)
  - GitHub: https://github.com/kimjeong1/Dodge_gamprogramming_26-1

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
     - 지금까지 누적된 정점 데이터를 모아 TRIANGLELIST 또는 LINELIST 형태로 GPU에 일괄 제출(Draw Call)합니다.
  6. gUITextObj::Render
     - UITextRenderer가 비트맵 폰트를 최상단에 Draw하여 실시간 UI 텍스트(점수, 상태 등)를 오버레이합니다.
  7. SwapChain::Present
     - 백버퍼의 최종 결과물을 전면 버퍼로 전환하여 화면에 출력합니다.

■ 리소스 최적화 및 디바이스 컨텍스트 관리
  - 셰이더 및 컴파일: D3DCompileFromFile을 통해 vertex shader(VS) 및 pixel shader(PS)를 런타임에 빌드합니다.
  - 버퍼 관리: 효율적인 데이터 전송을 위해 상수 버퍼(Constant Buffer)와 정점 버퍼(Vertex Buffer)를 관리합니다.
  - 상태 초기화: 애플리케이션 종료 시 Direct3D 디바이스(ID3D11Device), 컨텍스트(ID3D11DeviceContext), 스왑 체인 및 각종 렌더 타겟 뷰를 .Release() 호출을 통해 누수 없이 안전하게 해제합니다.


----------------------------------------------------------------------------------------------------------
3. 게임 요소 상세 (Game Elements)

■ 화면 해상도
  - 1280 x 720 (고정)

■ 플레이어
  - 이동 속도: 500px/s (방향키, deltaTime 기반)
    * 대각선 이동 시 약 707px/s (벡터 미정규화로 인한 자연스러운 속도 증가)
  - 크기: 24 x 24px

■ 대시 스킬 (스페이스바)
  - 이동 거리: 180px (마지막으로 이동한 방향으로 순간이동)
  - 쿨타임: 3초
  - 쿨타임 UI: 화면 좌측 하단 게이지 바(160 x 18px)로 시각화
  - 대시 시 잔상(DashTrail) 이펙트 표시

■ 장애물 (운석)
  - 크기: 18 x 18px
  - 등장 위치: 화면 4방향 가장자리(상/하/좌/우) 중 랜덤
  - 이동 방향: 화면 안쪽으로 진입하며 대각선 방향으로 랜덤 편차 포함
  - 기본 속도: 150 ~ 380px/s (랜덤)
  - 기본 스폰 간격: 1.2초마다 4개씩 등장
  - 최대 동시 존재 수: 500개
  - 화면 밖으로 이탈 시 오브젝트 풀로 반환 (재사용)

■ 무적 아이템 (별)
  - 스폰 간격: 15초마다 랜덤 위치에 1개 등장
  - 효과: 먹으면 5초간 무적 상태 (충돌 판정 무시)
  - 크기: 28 x 28px
  - 무적 중 플레이어 깜빡임 이펙트 표시

■ 난이도 시스템
  - 20초마다 자동으로 난이도 1단계 상승 (최대 6단계)
  - 단계가 오를수록 장애물 속도 증가, 스폰 간격 감소, 1회 스폰 수 증가


  | 난이도  | 경과시간 | 속도 범위 | 스폰간격 | 1회 스폰 수 |
-------------------------------------------------------------
  | 0단계   | 0초~     | 150~380   | 1.20초   | 4개         |
  | 1단계   | 20초~    | 190~440   | 1.07초   | 5개         |
  | 2단계   | 40초~    | 230~500   | 0.94초   | 6개         |
  | 3단계   | 60초~    | 270~560   | 0.80초   | 7개         |
  | 4단계   | 80초~    | 310~620   | 0.67초   | 8개         |
  | 5단계   | 100초~   | 350~680   | 0.54초   | 9개         |
  | 6단계   | 120초~   | 390~740   | 0.40초   | 10개        |



----------------------------------------------------------------------------------------------------------
4. 컴파일 및 실행 방법 (Compilation & Execution)

■ 시스템 요구 사양
  - OS: Windows 10 / 11 (64비트)
  - 그래픽 API: DirectX 11 (Windows 10/11 기본 내장, 별도 설치 불필요)
  - 런타임: Microsoft Visual C++ 2022 Redistributable (x64)
    * PC에 설치되어 있지 않은 경우 아래 링크에서 다운로드 후 설치
    * https://aka.ms/vs/17/release/vc_redist.x64.exe

■ 실행 방법
  1. 배포된 .exe 파일을 원하는 폴더에 저장합니다.
  2. .exe 파일을 더블클릭하여 즉시 실행합니다.

■ 조작 방법
  - [SPACE] : 메인 화면 시작 및 게임 진입
  - [방향키] : 플레이어 캐릭터 이동
  - [SPACE] : 대시(Dash) 특수 스킬 발동 (쿨타임 3초)
  - [ESC]   : 게임 종료
