all:
	@make -C ../../..

clean:
	@make -C ../../.. clean

stubs: fd_types.json
	python3 gen_stubs.py fd_types.h fd_types.c
