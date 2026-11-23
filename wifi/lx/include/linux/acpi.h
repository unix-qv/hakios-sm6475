#ifndef _LX_LINUX_ACPI_H
#define _LX_LINUX_ACPI_H
#include <linux/types.h>

#define ACPI_HANDLE(dev)		(NULL)
#define ACPI_COMPANION(dev)		(NULL)
#define ACPI_TYPE_INTEGER		0x01
#define ACPI_TYPE_PACKAGE		0x04
#define ACPI_ALLOCATE_BUFFER		((size_t)-1)
#define ACPI_FAILURE(s)			((s) != 0)

typedef void *acpi_handle;
typedef char *acpi_string;
typedef u32 acpi_status;
typedef u32 acpi_object_type;

struct acpi_buffer {
	size_t length;
	void *pointer;
};

union acpi_object {
	acpi_object_type type;
	struct {
		acpi_object_type type;
		u64 value;
	} integer;
	struct {
		acpi_object_type type;
		u32 count;
		union acpi_object *elements;
	} package;
};

static inline acpi_status
acpi_get_handle(acpi_handle parent, acpi_string pathname, acpi_handle *ret_handle)
{
	return (1);
}

static inline acpi_status
acpi_evaluate_object(acpi_handle object, acpi_string pathname, void * external_params, struct acpi_buffer *return_buffer)
{
	return (1);
}
#endif
