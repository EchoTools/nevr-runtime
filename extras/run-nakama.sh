#!/usr/bin/env bash
go run -C /home/andrew/src/nakama main.go \
    --name dev-nakama \
    --database.address "postgresql://postgres:localdb@localhost:5432/nakama?sslmode=disable" \
    --session.token_expiry_sec 7200 \
    --logger.level debug \
    --logger.file ./data/nakama.log \
    --data_dir ./data \
    --config ./local.yml
