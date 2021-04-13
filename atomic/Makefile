all:
	$(CC) -o atomic thread_pi.c main.c -g -lpthread -lm

one:
	./atomic

test:
	number=1 ; while [ $$number -le 1000 ] ; do\
	       ./atomic; echo $$number ;\
	       number=$$((number + 1)) ;\
	done

massif:
	valgrind --tool=massif ./atomic

perf:
	sudo perf stat --repeat 10000 -e cache-misses,cache-references,instructions,cycles ./atomic

address:
	gcc -o memory thread_pi.c main.c -g -lpthread -lm -fsanitize=address\
	& ./memory

clean:
	rm -rf atomic memory
