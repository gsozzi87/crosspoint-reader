#!/usr/bin/env bash
# Quién queda de administrador al registrarse. Sin base de datos.
set -e
cd "$(dirname "$0")/../../server"
bun test ../test/register_admin/admin.test.ts
