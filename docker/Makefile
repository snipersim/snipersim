UBUNTU_VERSION=16.04
DOCKER_IMAGE=ubuntu:$(UBUNTU_VERSION)-sniper-$(USER)
DOCKER_FILE=Dockerfile-ubuntu-$(UBUNTU_VERSION)

all: $(DOCKER_FILE)
	docker build -f $(DOCKER_FILE) -t $(DOCKER_IMAGE) .

run-root:
	docker run --privileged --rm -it -v "${HOME}:${HOME}" $(DOCKER_IMAGE)

run:
	docker run --privileged --rm -it -v "${HOME}:${HOME}" --user $(shell id -u):$(shell id -g) -w "${PWD}" $(DOCKER_IMAGE)

.PHONY: all
