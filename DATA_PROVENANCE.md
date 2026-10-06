# Data and source provenance

이 저장소는 “현재 논문과 현재 daemon 설계에 필요한 최소한의 재현 자료”를 남기는 것을 목표로 합니다. 연구 과정 전체의 실험 로그 보관소가 아닙니다.

## 포함한 것

- 현재 통합 `protected-daemon v2` source/test.
- 최종 측정 경로와 직접 관련된 DAMON 32-group validation source.
- controlled Step-C workload 및 동일-quota intervention runner.
- natural-workload fallback/intervention source와 3-placement × 4-block rotation runner.
- PMU raw 128-class cross-check source.
- 논문에서 사용한 핵심 결과를 사람이 검토하기 쉬운 compact CSV로 정리한 `results/`.
- 현재 논문 PDF.

## 의도적으로 제외한 것

- 수백 MB/GB가 될 수 있는 raw traces, per-process stdout/stderr, perf data, temporary cgroup output.
- calibration scratch output와 `kk-calibration-debug` archive.
- 이미 결론이 난 parameter sweep의 중복 raw data.
- build product, binary, object, skeleton, pycache.
- GAPBS graph와 외부 dependency source 자체.

## 연구 중 변경된 predictor 실험

연구 과정 중 후보 점수 가설은 여러 형태로 탐색되었습니다. 일부 중간 Step-C bundle에는 현재 논문의 `A × J`와 다른 exploratory fusion(예: PMU signal과 J를 결합한 형태)이 존재합니다. 이러한 bundle을 현재 최종 방법의 근거로 오해하지 않도록 본 clean repository에서는 제외했습니다.

현재 저장소에서 `A × J`는 통합 daemon의 현재 경로와 natural fallback experiment의 정의를 기준으로 합니다. `A`는 `proc_activity`의 bounded screening score이고, `J`는 DAMON `WEIGHTED_NORM` 32-group profile의 weighted Jaccard입니다.

## Compact result tables

`results/*.csv`는 기존 raw/console 출력에서 논문에 사용한 핵심 통계를 추려 보존한 것입니다. 새로운 실험을 수행해 만든 숫자가 아닙니다.

특히 synthetic candidate 결과는 현재 논문에 채택된 `A × J` 결과를 보존하지만, 연구 중 존재했던 모든 intermediate predictor bundle을 함께 배포하지는 않습니다. 향후 완전한 artifact release를 만들 경우 해당 결과를 생성한 정확한 run directory와 commit hash를 다시 결합하는 것을 권장합니다.

## 재현성 경계

일부 최종 결과의 original raw run directory나 마지막 wrapper script가 현재 업로드된 자료에 모두 남아 있지는 않습니다. 그런 경우 결과 CSV는 논문/보존된 console summary의 accepted values를 보존하며, 없는 source를 새로 만들어 “원래 실행 스크립트”라고 주장하지 않습니다.
