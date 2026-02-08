#!/bin/bash

SRC_FOLDER="src"
OUT_FOLDER="soundboard"

# Build file list with only audio files
files=()
for f in "$SRC_FOLDER"/*.wav "$SRC_FOLDER"/*.mp3 "$SRC_FOLDER"/*.flac "$SRC_FOLDER"/*.m4a ; do
  [[ -f "$f" ]] && files+=("$f")
done

if [[ ${#files[@]} -eq 0 ]]; then
  echo "No audio files found in $SRC_FOLDER"
  exit 1
fi

echo "Found ${#files[@]} audio files to normalize"

ffmpeg-normalize \
  --normalization-type rms \
  --target-level -18.0 \
  --loudness-range-target 36.0 \
  --true-peak -2.0 \
  --sample-rate 48000 \
  --audio-channels 1 \
  --verbose \
  --force \
  --extension wav \
  --output-folder "$OUT_FOLDER" \
  "${files[@]}"



  