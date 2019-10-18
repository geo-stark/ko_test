### Тестовый модуль ядра linux, реализующий хеш таблицу (ядро 3.7+)

Тестовый модуль ядра linux, реализующий хеш таблицу для работы из пользовательского окружения. Ключ и значение - целые числа (int). Максимальный размер размер таблицы по умолчанию 4096 элементов, размер можно изменить через параметр модуля hash_table_size, например  insmod ko_test.ko hash_table_size=100
Для взаимодействия с модулем предусмотрен интерфейс ioctl (определен в ko_test_ioctl.h, пример использования - test/main.c), а также через sysfs

#### Интерфейс sysfs:

Базовая директория:
/sys/devices/virtual/ko_test_class/ko_test_device/data

* файл add - добавление элемента, если его еще нет в списке, формат записи: <ключ>,<значение>
* файл set - добавление или изменение элемента, формат записи: <ключ>,<значение>
* файл delete - удаление элемента, формат записи: <ключ>

директория items содержит все элементы хеш таблицы, поддерживается изменение значений, формат записи: <ключ>

Тестировалось на ядре 5.2.18 x86_64 и 3.18 arm7