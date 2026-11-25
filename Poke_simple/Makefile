NAME   = jeu
RAYLIB = raylib/src
CFLAGS = -Wall -Wextra -I $(RAYLIB)

ifeq ($(OS),Windows_NT)
    NAME := $(NAME).exe
    LIBS  = -L $(RAYLIB) -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32
else
    LIBS  = -L $(RAYLIB) -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
endif

all: $(NAME)

$(RAYLIB)/libraylib.a:
	$(MAKE) -C $(RAYLIB) PLATFORM=PLATFORM_DESKTOP

$(NAME): main.c net.c net.h $(RAYLIB)/libraylib.a
	gcc $(CFLAGS) main.c net.c -o $(NAME) $(LIBS)

clean:
	rm -f $(NAME)
