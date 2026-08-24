#!/usr/bin/env bash
set -euo pipefail

BIN="${BIN:-./bin/oracle_natural_class_distribution}"
TRIALS="${TRIALS:-5}"
SIZES=(1 8 64)
STAMP="$(date +%Y%m%d_%H%M%S)"
RAW_DIR="raw"
RESULT_DIR="results"

mkdir -p "$RAW_DIR" "$RESULT_DIR"

DIST_TSV="$RESULT_DIR/g_natural_distribution_${STAMP}.tsv"
OVERLAP_TSV="$RESULT_DIR/g_natural_overlap_${STAMP}.tsv"

printf 'workset_mib\ttrial\tpool\ttotal_lines\tactive_classes\tclass_space\tcoverage_pct\tmax_share_pct\ttop16_share_pct\tnormalized_entropy\teffective_classes_hhi\tcv\tavg_classes_per_page\tmin_classes_per_page\tmax_classes_per_page\n' > "$DIST_TSV"
printf 'workset_mib\ttrial\tcandidate\tshared_active_classes\tunion_active_classes\tset_jaccard\tweighted_jaccard\tcosine_similarity\ttotal_variation\n' > "$OVERLAP_TSV"

for size in "${SIZES[@]}"; do
    log="$RAW_DIR/g_${size}mib_${STAMP}.log"
    echo "[RUN] workset=${size} MiB trials=${TRIALS}"
    sudo "$BIN" --workset-mib "$size" --trials "$TRIALS" | tee "$log"

    awk -F',' -v OFS='\t' -v s="$size" '
        /^RESULT_CSV,/ {
            # RESULT_CSV,trial,pool,workset_mib,total_lines,active_classes,class_space,
            # coverage,max_share,top16,entropy,effective,cv,avg_page,min_page,max_page
            print s,$2,$3,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16
        }
    ' "$log" >> "$DIST_TSV"

    awk -F',' -v OFS='\t' -v s="$size" '
        /^OVERLAP_CSV,/ {
            # OVERLAP_CSV,trial,candidate,shared,union,set_jaccard,weighted,cosine,tv
            print s,$2,$3,$4,$5,$6,$7,$8,$9
        }
    ' "$log" >> "$OVERLAP_TSV"
done

echo
printf '[SAVED] raw logs: %s/g_*_%s.log\n' "$RAW_DIR" "$STAMP"
printf '[SAVED] distribution TSV: %s\n' "$DIST_TSV"
printf '[SAVED] overlap TSV: %s\n' "$OVERLAP_TSV"
