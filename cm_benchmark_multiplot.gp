set title "Benchmark ConfigurationModel"

# Output
set output 'cm_benchmark_mout.pdf'

# Legend location
set key left top

# Terminal output
set terminal pdf

# Linestyles
set style line 1 lc rgb '#0060ad' lt 1 lw 2 pt 3 ps 1.5   # --- blue
set style line 2 lc rgb '#ff0000' lt 1 lw 2 pt 3 ps 1.5   # --- red
set style line 3 lc rgb '#000000' lt 1 lw 2 pt 3 ps 1.5   # --- black
set style line 4 lc rgb '#006400' lt 1 lw 2 pt 3 ps 1.5   # --- dark green

# Edges - Runtime
set xlabel "Edges"
set ylabel "Time [s]"
set logscale xy

plot 'runtime_cm_crc.dat' using 2:3 with linespoints ls 1 title "CRC", \
     'runtime_cm_r.dat' using 2:3 with linespoints ls 2 title "Random" \
     'runtime_cm_tupr.dat' using 2:3 with linespoints ls 4 title "Random2"

set title "Self-loops proportions comparison"
set key right top

#############################################################
# Edges - Self-loops percent
set xlabel "Edges"
set ylabel "Self-loops"
set autoscale
set logscale x

plot 'runtime_cm_crc.dat' using 1:($4/$2) with linespoints ls 1 title "CRC", \
     'runtime_cm_r.dat' using 1:($4/$2) with linespoints ls 2 title "Random" \
     'runtime_cm_tupr.dat' using 1:($4/$2) with linespoints ls 4 title "Random2"

set title "Self-loops absolute comparison vs. Theory"
set key left top

################################################################
# Nodes - Self-loops percent vs Theory
set xlabel "Nodes"
set ylabel "Self-loops"
set autoscale
set logscale x

# CAREFUL HARDCODED HERE
# MIN_DEG = 5
MIN_DEG = 5
Upper(x) = 0.5*((x/10 - MIN_DEG + 1)/(log(x/10 + 1) - log(MIN_DEG)))

plot 'runtime_cm_crc.dat' using 1:4 with linespoints ls 1 title "CRC", \
     'runtime_cm_r.dat' using 1:4 with linespoints ls 2 title "Random", \
     'runtime_cm_tupr.dat' using 1:($4/$2) with linespoints ls 4 title "Random2"
     Upper(x) title "Upperbound w.h.p." with lines ls 3

################################################
# Edges - Multiedges
set title "Multi-edges absolute comparison"
set key right top

set xlabel "Edges"
set ylabel "Multi-edges"
set autoscale
set logscale x

plot 'runtime_cm_crc.dat' using 2:5 with linespoints ls 1 title "CRC", \
     'runtime_cm_r.dat' using 2:5 with linespoints ls 2 title "Random" \
     'runtime_cm_tupr.dat' using 1:($4/$2) with linespoints ls 4 title "Random2"

################################################
# Edges - Multiedges percentage
set title "Multi-edges quantity proportions comparison"
set key right top

set xlabel "Edges"
set ylabel "Multi-edges quantity"
set autoscale
set logscale x

plot 'runtime_cm_crc.dat' using 2:($6/$2) with linespoints ls 1 title "CRC", \
     'runtime_cm_r.dat' using 2:($6/$2) with linespoints ls 2 title "Random" \
     'runtime_cm_tupr.dat' using 1:($4/$2) with linespoints ls 4 title "Random2"