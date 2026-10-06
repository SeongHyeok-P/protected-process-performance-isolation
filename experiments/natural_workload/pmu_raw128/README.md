# PMU raw-128 cross-check

`oracle_natural_class_distribution_v2.c`는 물리주소가 포함된 PMU sample을 recovered raw class 축으로 분류하는 검증 코드입니다. 논문에서는 32 page-observable group에서 관찰된 평탄화가 단순히 DAMON page granularity 때문에 생긴 것인지 점검하기 위한 보조 검증으로 사용했습니다.

PMU sample은 전체 memory traffic의 ground truth가 아닙니다. 특히 store, prefetch, 모든 DRAM request, 정확한 request concurrency/row-buffer 상태를 모두 나타내지 않습니다.

이 소스는 `calibration.h`와 `dram_mapping.h`를 필요로 하므로 필요 시 `protected-daemon/v2/src` 또는 검증 bundle의 대응 구현을 include/link 하십시오.
