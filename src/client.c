#include <uv.h>
#include <stdio.h>
#include <stdlib.h>

#include "plasma.h"

struct sockaddr_in addr;
uv_tcp_t tcp;

void echo_read(uv_stream_t *server, ssize_t nread, const uv_buf_t *buf) {
  if (nread == -1) {
    fprintf(stderr, "error echo_read");
    return;
  }
  printf("result: %s\n", buf->base);
  uv_stop(uv_default_loop());
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

void on_write_end(uv_write_t *req, int status) {
  if (status == -1) {
    fprintf(stderr, "error on_write_end");
    return;
  }
  uv_read_start(req->handle, alloc_buffer, echo_read);
}

void on_connect(uv_connect_t* req, int status) {
	plasma_request request;
	request.type = 42;
	uv_buf_t buf = uv_buf_init((char*) &request, sizeof(request));
	buf.len = sizeof(request);
	buf.base = (char*) &request;
	uv_stream_t *tcp = req->handle;
	uv_write_t write_req;
	uv_write(&write_req, tcp, &buf, 1, on_write_end);
}

int main() {
  uv_loop_t *loop = uv_default_loop();
	
  uv_tcp_init(loop, &tcp);

	uv_ip4_addr("0.0.0.0", 7000, &addr);
	
	uv_connect_t connect_req;
	
  uv_tcp_connect(&connect_req, &tcp, (struct sockaddr*) &addr, on_connect);
  
  uv_run(loop, UV_RUN_DEFAULT);
}
