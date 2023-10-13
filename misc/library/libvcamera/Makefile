CFLAGS = -fPIC

VCAMHAL_SRC = vICamera.cpp
VCAMHAL_OBJ = vICamera.o
VCAMHAL_LIB = libvcamhal.so

VCAMHAL_TEST_SRC = vcamhal_test.c
VCAMHAL_TEST_OBJ = $(VCAMHAL_TEST_SRC:.c=.o)
VCAMHAL_TEST = vcamhal_test.bin

ICAMHAL_TEST_SRC = icamhal_test.cpp
ICAMHAL_TEST_OBJ = $(ICAMHAL_TEST_SRC:.cpp=.o)
ICAMHAL_TEST = icamhal_test.bin

LDFLAGS = -L. -lcamhal

all: $(VCAMHAL_OBJ) $(VCAMHAL_LIB) $(VCAMHAL_TEST) $(ICAMHAL_TEST)

$(VCAMHAL_OBJ): $(VCAMHAL_SRC)
	$(CXX) $(CFLAGS) $^ -c -o $@

$(VCAMHAL_LIB): $(VCAMHAL_OBJ)
	$(CXX) -shared $^ -o $@ $(LDFLAGS)

$(VCAMHAL_TEST): $(VCAMHAL_TEST_OBJ) $(VCAMHAL_LIB)
	$(CC) $^ -o $@

$(ICAMHAL_TEST): $(ICAMHAL_TEST_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

install: $(VCAMHAL_LIB)
	sudo cp $(VCAMHAL_LIB) /usr/lib/

clean:
	rm -f *.o *.so *.bin

.PHONY: clean install
