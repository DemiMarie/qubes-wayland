#!/bin/sh --
set -eu
unset depfile outdir target_dir path mode
depfile=$1 outdir=$2 target_dir=$3 path=$4 mode=$5
shift 5
case $mode in
(debug|release) :;;
(*) echo 'Mode must be debug or release'>&2; exit 1;;
esac
cargo build "--target-dir=$target_dir" "--$mode" "--manifest-path=$path/Cargo.toml" "$@" &&
cp -- "$target_dir/$mode/libqubes_gui_rust.a" "$outdir/" &&
cp -- "$target_dir/$mode/libqubes_gui_rust.d" "$depfile"
