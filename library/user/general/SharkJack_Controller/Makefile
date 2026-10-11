# Build the Shark Jack Controller for the Pager (MT7628, MIPS 24KEc, soft-float).
BINARY = sharkjack-controller
GOOS   = linux
GOARCH = mipsle
GOMIPS = softfloat
LDFLAGS = -s -w

.PHONY: build clean
build:
	GOOS=$(GOOS) GOARCH=$(GOARCH) GOMIPS=$(GOMIPS) CGO_ENABLED=0 \
		go build -trimpath -ldflags="$(LDFLAGS)" -o $(BINARY) .
	@echo "size: $$(ls -l $(BINARY) | awk '{print $$5}') bytes"
	@shasum -a 256 $(BINARY)

clean:
	rm -f $(BINARY)
