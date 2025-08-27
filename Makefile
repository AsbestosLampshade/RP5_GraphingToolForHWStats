hello:
	gcc $(pkg-config --cflags gtk4) -o hw_stats hw_stats.c $(pkg-config --libs gtk4)
