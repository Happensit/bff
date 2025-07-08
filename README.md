Как собрать и запустить

Установите необходимые зависимости (библиотеку glib-2.0 и http-parser).

На Debian/Ubuntu: sudo apt-get install libglib2.0-dev libhttp-parser-dev

Сохраните все файлы в одной директории.

Выполните команду make.

Запустите сервер: ./server

Проверьте его работу: curl http://localhost:8080/health
