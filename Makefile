CXX ?= c++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
NAME := TrpCleaner

.PHONY: all install uninstall clean fclean re test
all: $(NAME)

$(NAME): src/main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -D_FILE_OFFSET_BITS=64 $< $(LDFLAGS) -o $@

install: $(NAME)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 $(NAME) "$(DESTDIR)$(BINDIR)/$(NAME)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(NAME)"

tests/safety: tests/safety.cpp src/main.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++17 -D_FILE_OFFSET_BITS=64 $< $(LDFLAGS) -o $@

test: tests/safety
	./tests/safety

clean:
	rm -f tests/safety

fclean: clean
	rm -f $(NAME)

re:
	$(MAKE) fclean
	$(MAKE) all
