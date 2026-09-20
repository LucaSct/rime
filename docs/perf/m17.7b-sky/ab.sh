set -u
R=/home/next/projects/rime
S=/tmp/claude-1000/-home-next-projects-rime/31fdb0fc-da09-4f64-a4cf-caa2a57f2417/scratchpad
F=$R/samples/99-the-block/main.cpp
cp "$F" $S/main.cpp.abbackup
set_arm() {   # $1 = on|off
  if [ "$1" = off ]; then
    sed -i 's/^        sky.enabled = true;$/        sky.enabled = false; \/\/ AB-ARM-OFF/' "$F"
  else
    sed -i 's/^        sky.enabled = false; \/\/ AB-ARM-OFF$/        sky.enabled = true;/' "$F"
  fi
  cmake --build --preset release > /dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
}
cd $R
rm -f $S/ab-results.txt
for i in 1 2 3; do
  for arm in on off; do
    set_arm $arm
    ( cd $R/build/release/samples/99-the-block && $R/build/release/bin/the_block --perf --frames 240 > $S/ab-$arm-$i.log 2>&1 )
    render_p50=$(grep -oP 'render\s+p50 \K[0-9.]+' $S/ab-$arm-$i.log | head -1)
    render_p99=$(grep -oP 'render\s+p50 [0-9.]+\s+p99 \K[0-9.]+' $S/ab-$arm-$i.log | head -1)
    player_p99=$(grep -oP 'frame.player p50 [0-9.]+\s+p99 \K[0-9.]+' $S/ab-$arm-$i.log | head -1)
    echo "$arm $i render_p50=$render_p50 render_p99=$render_p99 player_p99=$player_p99" >> $S/ab-results.txt
    echo "  [$arm run $i] render p50=$render_p50 p99=$render_p99  player p99=$player_p99"
  done
done
cp $S/main.cpp.abbackup "$F"
cmake --build --preset release > /dev/null 2>&1
echo "=== restored; sky.enabled line is now: $(grep -n 'sky.enabled = ' "$F" | head -1) ==="
