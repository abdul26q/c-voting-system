FROM gcc:latest

WORKDIR /app

COPY voting_server.c .

RUN gcc voting_server.c -o voting_server

EXPOSE 8080

CMD ["./voting_server"]
