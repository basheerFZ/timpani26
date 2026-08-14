<!--
SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
SPDX-License-Identifier: MIT
-->

# TIMPANI Version Management Guide

이 문서는 TIMPANI 프로젝트의 버전 관리 정책과 릴리스 프로세스를 설명합니다.

## Calendar Versioning (CalVer)

TIMPANI는 [Calendar Versioning](https://calver.org/)을 사용합니다.

### 버전 형식

```
YYYY.MM.PATCH
```

| 구성요소 | 설명 | 예시 |
|:--|:--|:--|
| **YYYY** | 연도 (4자리) | 2026 |
| **MM** | 월 (1-12, 선행 0 없음) | 4 |
| **PATCH** | 해당 월 내 패치 번호 (0부터 시작) | 0, 1, 2... |

### 버전 예시

| 버전 | 의미 |
|:--|:--|
| `2026.04.0` | 2026년 4월 첫 릴리스 |
| `2026.04.1` | 2026년 4월 첫 패치 |
| `2026.05.0` | 2026년 5월 첫 릴리스 |

---

## 파일 구조

```
TIMPANI/
├── VERSION              # 단일 버전 소스 (예: 2026.04.1)
├── CHANGELOG.md         # 변경 이력
├── timpani-o/
│   └── CMakeLists.txt   # file(READ "../VERSION" ...)
├── timpani-n/
│   └── CMakeLists.txt   # file(READ "../VERSION" ...)
└── sample-apps/
    └── CMakeLists.txt   # file(READ "../VERSION" ...)
```

### VERSION 파일

- **위치**: 프로젝트 루트 (`/VERSION`)
- **형식**: 버전 문자열만 포함 (줄바꿈 포함)
- **예시**:
  ```
  2026.04.1
  ```

### CMake 통합

각 컴포넌트의 CMakeLists.txt에서 루트 VERSION 파일을 참조합니다:

```cmake
# Read version from project root VERSION file
file(READ "${CMAKE_SOURCE_DIR}/../VERSION" VERSION_CONTENT)
string(STRIP "${VERSION_CONTENT}" PROJECT_VERSION_STRING)

project(timpani-o VERSION ${PROJECT_VERSION_STRING} ...)
```

---

## CHANGELOG 작성법

[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) 형식을 따릅니다.

### 구조

```markdown
## [Unreleased]

## [YYYY.MM.PATCH] - YYYY-MM-DD

### 컴포넌트명 (timpani-o, timpani-n, sample-apps)

#### Added
- 새로운 기능

#### Changed
- 변경된 기능

#### Fixed
- 버그 수정

#### Removed
- 제거된 기능
```

### 작성 규칙

1. **컴포넌트별 구분**: timpani-o, timpani-n, sample-apps 각각 섹션 분리
2. **커밋 해시 포함**: 중요 변경사항에 커밋 해시 명시 (예: `aa797bc`)
3. **간결한 설명**: 무엇이 변경되었는지 한 줄로 설명
4. **[Unreleased]**: 릴리스 전 변경사항은 여기에 먼저 작성

---

## 릴리스 프로세스

### 1. 버전 결정

| 상황 | 버전 변경 |
|:--|:--|
| 새로운 달의 첫 릴리스 | `YYYY.MM.0` |
| 같은 달 내 패치/기능 추가 | `YYYY.MM.PATCH+1` |

### 2. 파일 업데이트

```bash
# 1. VERSION 파일 업데이트
echo "2026.04.1" > VERSION

# 2. CHANGELOG.md 업데이트
# - [Unreleased] 내용을 새 버전 섹션으로 이동
# - 날짜 추가
```

### 3. 커밋 및 태그

```bash
# 커밋
git add VERSION CHANGELOG.md
git commit -m "chore(release): bump version to 2026.04.1

- Update VERSION: 2026.04.0 → 2026.04.1
- Update CHANGELOG: Add [2026.04.1] section"

# 태그 (annotated — 태거/날짜/메시지 메타데이터 포함)
git tag -a v2026.04.1 -m "TIMPANI 2026.04.1"

# 태그 푸시 (선택)
git push origin v2026.04.1
```

### 4. 검증

```bash
# 태그 확인
git tag -l "v2026*"

# 버전 확인 (빌드 후)
./timpani-n --version  # (구현 시)
```

---

## 커밋 메시지 컨벤션

[Conventional Commits](https://www.conventionalcommits.org/) 형식을 따릅니다.

### 형식

```
<type>(<scope>): <description>

[optional body]

[optional footer]
```

### Type

| Type | 설명 | CHANGELOG 섹션 |
|:--|:--|:--|
| `feat` | 새로운 기능 | Added |
| `fix` | 버그 수정 | Fixed |
| `docs` | 문서 변경 | - |
| `refactor` | 리팩토링 | Changed |
| `chore` | 유지보수 (빌드, CI 등) | - |
| `test` | 테스트 추가/수정 | - |
| `perf` | 성능 개선 | Changed |

### Scope

| Scope | 대상 |
|:--|:--|
| `timpani-o` | Orchestrator |
| `timpani-n` | Node Executor |
| `sample-apps` | Sample Applications |
| `release` | 버전/릴리스 관련 |
| (생략) | 프로젝트 전체 |

### 예시

```
feat(timpani-n): add systemd service unit for production deployment

fix(timpani-o): push schedule table to connected nodes instead of self

chore(release): bump version to 2026.04.1

docs: update README with container deployment guide
```

---

## Git 태그 규칙

### 형식

```
v{VERSION}
```

예: `v2026.04.0`, `v2026.04.1`

### Annotated 태그 사용

릴리스 태그는 **annotated 태그**(`git tag -a`)로 생성합니다.

- **이유**: 태거(who), 날짜(when), 메시지(why)가 태그 객체에 함께 저장되어
  릴리스 추적성이 높고, `git describe`와 GitHub Release 노트 연동에 유리합니다.
- lightweight 태그(`git tag <name>`)는 커밋을 가리키는 단순 포인터라 메타데이터가 없습니다.
- **종류 확인**: `git cat-file -t v2026.08.0` → annotated는 `tag`, lightweight는 `commit`.

> 참고: `v2026.08.0` 이전 태그(`v2026.04.*`, `v2026.07.0`)는 lightweight로 생성되어
> 이미 원격에 공개되어 있습니다. 공개된 태그의 소급 변환(force-push)은 지양하며,
> `v2026.08.0`부터 annotated 관례를 적용합니다.

### 태그 관리

```bash
# 태그 목록 확인 (메시지 포함)
git tag -n99 -l "v2026*"

# annotated 태그 생성 (현재 커밋)
git tag -a v2026.04.0 -m "TIMPANI 2026.04.0"

# 특정 커밋에 annotated 태그 추가 (나중에)
git tag -a v2026.04.0 -m "TIMPANI 2026.04.0" d5752d4

# 태그 정보 확인
git show v2026.04.0

# 태그 삭제 (실수 시)
git tag -d v2026.04.0

# 원격 태그 삭제
git push origin --delete v2026.04.0
```

---

## FAQ

### Q: 버전을 언제 올려야 하나요?

**A**: 다음 상황에서 버전을 올립니다:
- 새로운 기능 릴리스
- 버그 수정 배포
- 호환성에 영향을 주는 변경

개발 중인 변경사항은 `[Unreleased]` 섹션에 기록하고, 릴리스 시점에 버전을 부여합니다.

### Q: 컴포넌트별로 다른 버전을 사용할 수 없나요?

**A**: TIMPANI는 단일 버전 정책을 사용합니다.
- **이유**: timpani-o와 timpani-n은 gRPC proto를 공유하며 강하게 결합됨
- **장점**: "TIMPANI 2026.04.x면 모든 컴포넌트 호환" 보장
- **CHANGELOG**: 컴포넌트별 변경사항은 CHANGELOG에서 구분

### Q: 태그는 언제 생성하나요?

**A**: VERSION 파일을 업데이트하는 커밋에 태그를 붙입니다.
```bash
git commit -m "chore(release): bump version to 2026.04.1"
git tag -a v2026.04.1 -m "TIMPANI 2026.04.1"  # 방금 커밋에 annotated 태그
```

---

## 참고 자료

- [Calendar Versioning](https://calver.org/)
- [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
- [Conventional Commits](https://www.conventionalcommits.org/)
