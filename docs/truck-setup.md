# Euro Truck Simulator 2 / American Truck Simulator

OBS 플러그인이 게임 텔레메트리를 직접 읽습니다. SCS 게임은 게임 폴더 안에 작은 텔레메트리 DLL을 설치해야 합니다. 별도 실행 프로그램, SimHub, UDP 포트 설정은 필요 없습니다.

1. 게임을 종료합니다.
2. Steam에서 게임 → 속성 → 설치된 파일 → 찾아보기를 엽니다.
3. 게임 폴더의 `bin\win_x64\plugins` 폴더를 만듭니다(이미 있으면 그대로 사용).
4. 배포 파일의 `truck\racing-motion-scs.dll`을 그 폴더에 복사합니다.
5. 게임을 시작하고 텔레메트리 SDK 사용 알림이 표시되면 확인합니다.
6. OBS의 Racing Motion 필터에서 `Euro Truck Simulator 2` 또는 `American Truck Simulator`를 선택하고 실제 운전을 시작합니다.

예시 경로:

```text
...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\racing-motion-scs.dll
...\steamapps\common\American Truck Simulator\bin\win_x64\plugins\racing-motion-scs.dll
```

두 게임에 동일한 DLL을 설치할 수 있습니다. 게임 ID를 확인해서 서로 다른 메모리를 사용합니다. OBS용 `obs-racing-motion.dll`은 OBS에, `racing-motion-scs.dll`은 게임에 설치합니다.

메뉴·일시정지·게임 종료에서는 비활성 상태를 전달합니다. 게임이 강제 종료되거나 데이터가 끊기면 OBS 수신기의 시간 제한으로 움직임을 중단합니다. 필터의 축 반전 설정으로 화면에 맞는 방향을 조정할 수 있습니다.

연결되지 않으면 게임의 `game.log.txt`에서 `OBS Racing Motion: native truck telemetry initialized` 문구를 확인하세요. DLL 위치와 x64 실행 여부를 확인하고 게임과 OBS를 같은 Windows 사용자 세션에서 실행하세요. 게임 재시작 후 다시 운전 상태로 진입합니다.

## 데이터 계약 및 검증 범위

게임 DLL은 SCS 텔레메트리 API만 등록합니다. 입력 장치 API를 구현하지 않으며 조향·브레이크 등 게임 조작은 하지 않습니다.

공식 SDK의 `scssdk_value.h`는 로컬 좌표를 X=오른쪽, Y=위, Z=뒤로 정의합니다. `scssdk_telemetry_truck_common_channels.h`의 선형 가속도 단위는 m/s²입니다. 다음 변환을 적용합니다.

| 출력 | SCS 채널/성분 | 변환 |
|---|---|---|
| surge | local_linear_acceleration.z | 부호 반전: 앞쪽 가속이 양수 |
| sway | local_linear_acceleration.x | 그대로: 오른쪽 양수 |
| heave | local_linear_acceleration.y | 그대로: 위쪽 양수 |
| pitch, roll | world_placement.orientation | 회전수 × 2π → 라디안 |
| yaw | world_placement.orientation.heading | 한 바퀴 범위로 정규화 후 라디안 |
| speed | speed | 절댓값 × 3.6 → km/h |
| rpm, gear | engine_rpm, engine_gear | RPM / 부호 있는 기어 번호 |

SDK는 이 선형 가속도 채널에 별도의 중력 오프셋을 명시하지 않습니다. 이 구현은 문서에 없는 9.81 보정을 추가하지 않습니다. 실제 게임에서의 정지·경사·충돌 상태 감각은 추가 현장 확인이 필요합니다.

공유 메모리는 m/s² 단위를 사용하며 OBS 수신 단계에서 9.81로 나누어 공통 텔레메트리의 G 단위로 변환합니다.

각 프레임의 가속도와 자세가 유효할 때만 활성 데이터를 발행합니다. NaN/무한대 및 범위를 벗어난 값은 거부합니다. 부가 정보가 없는 프레임은 해당 부가 정보를 0으로 표시합니다. 프레임 데이터는 72바이트 고정 크기이고 magic/version/size, 활성 플래그, 표본 번호 및 `GetTickCount64()` 시각을 포함합니다.

공유 메모리 이름은 `Local\OBSRacingMotionETS2_v1` 및 `Local\OBSRacingMotionATS_v1`입니다. 같은 이름에 `_Mutex`를 붙인 Windows mutex로 프레임 전체 복사를 보호합니다. 게임과 OBS 모두 대기 시간 0으로 잠금을 시도하여 충돌 시 해당 표본을 건너뜁니다. 비정상 종료로 포기된 잠금을 발견한 OBS 리더는 해당 표본을 버리고 다음 표본을 기다립니다.

자동 테스트는 실제 x64 DLL을 로드하고 SCS 콜백을 모사하여 초기화 실패, 프레임 완성 시점, 축/단위 변환, 잘못된 값, 잠금 충돌·포기, 일시정지, 종료 및 ETS2/ATS 메모리 구분을 확인합니다. 실제 ETS2/ATS 주행 통합 테스트를 수행했다는 의미는 아닙니다.

## 공식 자료 / 빌드

- [SCS Telemetry SDK 공식 페이지](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry)
- [SDK 1.14 ZIP](https://download.eurotrucksimulator2.com/scs_sdk_1_14.zip)
- 다운로드 SHA-256: `c6c1f7376b7324994d9f9c567f3c4141fbbf305b6bf803bc4cfeef2437b2023a`
- 사용 API: Telemetry 1.00 / 1.01 (`SDK 1.14`는 배포 아카이브 버전)
- 소스: `truck/truck_telemetry.cpp`, `truck/truck_telemetry.def`
- 헤더 경로: `.deps/scs-sdk/include`
- 링크: Windows Kernel32, C++17; 외부 서비스나 SCS import library 불필요
- SDK 라이선스: `.deps/scs-sdk/sdk_license.txt` (배포에 고지문 포함)

개별 DLL 및 콜백 테스트 빌드 예시(저장소 루트, LLVM-MinGW의 bin을 PATH에 추가):

```powershell
rtk proxy clang++.exe -std=c++17 -O2 -Wall -Wextra -Werror -static -shared -I.deps/scs-sdk/include truck/truck_telemetry.cpp truck/truck_telemetry.def -o .tools/racing-motion-scs.dll
rtk proxy clang++.exe -std=c++17 -O2 -Wall -Wextra -Werror -static -I.deps/scs-sdk/include tests/truck_test.cpp -o .tools/truck_test.exe
rtk proxy .tools/truck_test.exe .tools/racing-motion-scs.dll
```

SDK를 다시 내려받을 때는 위 공식 ZIP URL을 사용하고 SHA-256 일치를 확인한 다음 `.deps/scs-sdk`에 압축을 해제합니다. 배포에는 `truck/SCS-SDK-LICENSE.txt`를 포함합니다. SDK 헤더 또는 컴파일러 DLL을 게임에 복사할 필요는 없습니다.
