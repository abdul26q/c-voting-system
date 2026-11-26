FROM gcc:latest
WORKDIR /app
COPY voting_system.c .
RUN gcc voting_system.c -o voting_system
EXPOSE 8080
CMD ["./voting_system"]
