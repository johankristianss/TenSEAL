IMAGE ?= johan/tenseal-jupyter

build:
	docker build -t $(IMAGE) . 
push:
	docker push $(IMAGE)
run:
	#docker run -v /root/notebook/:/notebook -it -p 28888:8888 $(IMAGE)
	docker run -it -p 28888:8888 $(IMAGE)

shell:
	#docker run -v $(HOME):/root/notebook -it -p 28888:8888 $(IMAGE) /bin/bash
	docker run -it -p 28888:8888 $(IMAGE) /bin/bash
