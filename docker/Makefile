
DOCKER_IMAGE=ubuntu:16.04-sniper-$(USER)

all: Dockerfile
	docker build -t $(DOCKER_IMAGE) .

run-root:
	docker run --privileged --rm -it -v "${HOME}:${HOME}" $(DOCKER_IMAGE)

run:
	docker run --privileged --rm -it -v "${HOME}:${HOME}" --user $(shell id -u):$(shell id -g) $(DOCKER_IMAGE)

.PHONY: all
