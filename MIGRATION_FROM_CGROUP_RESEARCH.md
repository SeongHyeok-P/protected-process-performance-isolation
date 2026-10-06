# 기존 cgroup_research GitHub 저장소 교체 절차

권장 repository 이름:

```text
protected-process-performance-isolation
```

기존 `cgroup_research` history를 실수로 잃지 않도록 먼저 backup branch를 remote에 남긴 뒤 main을 이 clean snapshot으로 교체하는 방법입니다.

## 1. 기존 저장소 백업

```bash
cd ~/cgroup_research

git status
git add -A
git commit -m "Backup before repository cleanup"   # 변경사항이 있을 때만

git branch archive/cgroup-research-before-cleanup
git push origin archive/cgroup-research-before-cleanup

REMOTE_URL=$(git remote get-url origin)
echo "$REMOTE_URL"
```

## 2. 이 clean snapshot을 새 로컬 디렉터리에 배치

압축파일을 홈 디렉터리에 풀었다고 가정합니다.

```bash
cd ~/protected-process-performance-isolation

git init -b main
git remote add origin "$REMOTE_URL"
git fetch origin main

git add .
git commit -m "Reorganize research repository for protected-process performance isolation"
```

## 3. 기존 GitHub main 교체

backup branch가 GitHub에 올라간 것을 확인한 뒤:

```bash
git push --force-with-lease -u origin main
```

`--force`보다 `--force-with-lease`를 권장합니다.

## 4. GitHub repository 이름도 변경하려는 경우

GitHub 웹에서 기존 repository의 **Settings → General → Repository name**에서:

```text
cgroup_research
```

를

```text
protected-process-performance-isolation
```

로 변경합니다. 이후 GitHub가 표시하는 새 URL로 local remote를 갱신합니다.

```bash
git remote set-url origin <새 GitHub repository URL>
git remote -v
```

## 5. 기존 로컬 폴더 처리

새 저장소가 정상 push된 뒤에만 기존 로컬 폴더를 지우거나 archive하십시오.

```bash
mv ~/cgroup_research ~/cgroup_research_archive
```

원래 실험 raw data가 필요할 수 있으므로 당분간 삭제보다 archive를 권장합니다.

## 이름 변경에 따른 한 가지 차이

repository directory 이름과 별개로 daemon 내부 cgroup runtime namespace도 이 clean snapshot에서는 다음처럼 정리했습니다.

```text
/sys/fs/cgroup/protected_process_isolation
```

기존 `/sys/fs/cgroup/cgroup_research`를 자동으로 migration하지 않으므로 실제 `--apply` 전에 cgroup 준비 스크립트/환경과 맞는지 확인하십시오.
