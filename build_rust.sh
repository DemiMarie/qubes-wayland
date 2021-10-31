#!/bin/sh --
set -eu
unset outdir target_dir mode
depfile=$1 outdir=$2 target_dir=$3 mode=$4
shift 4
case $mode in
(debug|release) :;;
(*) echo 'Mode must be debug or release'>&2; exit 1;;
esac
cargo build "--target-dir=$target_dir" "--$mode" "$@" &&
cp -- "$target_dir/$mode/libqubes_gui_rust.a" "$outdir/" &&
cp -- "$target_dir/$mode/libqubes_gui_rust.d" "$depfile"
