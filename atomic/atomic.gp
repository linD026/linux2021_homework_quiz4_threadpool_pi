reset
set title "atomic thread pool 100"
set xlabel "n ( threads )"
set ylabel "nanosecond"
set log y 10
#set format y '%l'
#set ytics 0, 1, 10
#set yrange [100000:10000000]
#set yrange [500000:20000000]
#set ytics 100000, 10000, 5000000
set terminal png font "threadpool"
set output "thread_pool_100_check.png"
set key left
plot\
"bpp_benckmark_atomic.txt"  using 1:2 with lines title "atomic",\
"bpp_benckmark_normal.txt" using 1:2 with lines title "normal",\
