


#define ACPI_CREATE_PREDEFINED_TABLE

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "acpredef.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nspredef")

/* Local prototypes */
static acpi_status
acpi_ns_check_package(struct acpi_predefined_data *data,
		      union acpi_operand_object **return_object_ptr);

static acpi_status
acpi_ns_check_package_list(struct acpi_predefined_data *data,
			   const union acpi_predefined_info *package,
			   union acpi_operand_object **elements, u32 count);

static acpi_status
acpi_ns_check_package_elements(struct acpi_predefined_data *data,
			       union acpi_operand_object **elements,
			       u8 type1,
			       u32 count1,
			       u8 type2, u32 count2, u32 start_index);

static acpi_status
acpi_ns_check_object_type(struct acpi_predefined_data *data,
			  union acpi_operand_object **return_object_ptr,
			  u32 expected_btypes, u32 package_index);

static acpi_status
acpi_ns_check_reference(struct acpi_predefined_data *data,
			union acpi_operand_object *return_object);

static void acpi_ns_get_expected_types(char *buffer, u32 expected_btypes);

static const char *acpi_rtype_names[] = {
	"/Integer",
	"/String",
	"/Buffer",
	"/Package",
	"/Reference",
};


acpi_status
acpi_ns_check_predefined_names(struct acpi_namespace_node *node,
			       u32 user_param_count,
			       acpi_status return_status,
			       union acpi_operand_object **return_object_ptr)
{
	union acpi_operand_object *return_object = *return_object_ptr;
	acpi_status status = AE_OK;
	const union acpi_predefined_info *predefined;
	char *pathname;
	struct acpi_predefined_data *data;

	/* Match the name for this method/object against the predefined list */

	predefined = acpi_ns_check_for_predefined_name(node);

	/* Get the full pathname to the object, for use in warning messages */

	pathname = acpi_ns_get_external_pathname(node);
	if (!pathname) {
		return AE_OK;	/* Could not get pathname, ignore */
	}

	/*
	 * Check that the parameter count for this method matches the ASL
	 * definition. For predefined names, ensure that both the caller and
	 * the method itself are in accordance with the ACPI specification.
	 */
	acpi_ns_check_parameter_count(pathname, node, user_param_count,
				      predefined);

	/* If not a predefined name, we cannot validate the return object */

	if (!predefined) {
		goto cleanup;
	}

	/*
	 * If the method failed or did not actually return an object, we cannot
	 * validate the return object
	 */
	if ((return_status != AE_OK) && (return_status != AE_CTRL_RETURN_VALUE)) {
		goto cleanup;
	}

	/*
	 * If there is no return value, check if we require a return value for
	 * this predefined name. Either one return value is expected, or none,
	 * for both methods and other objects.
	 *
	 * Exit now if there is no return object. Warning if one was expected.
	 */
	if (!return_object) {
		if ((predefined->info.expected_btypes) &&
		    (!(predefined->info.expected_btypes & ACPI_RTYPE_NONE))) {
			ACPI_WARN_PREDEFINED((AE_INFO, pathname,
					      ACPI_WARN_ALWAYS,
					      "Missing expected return value"));

			status = AE_AML_NO_RETURN_VALUE;
		}
		goto cleanup;
	}

	/*
	 * 1) We have a return value, but if one wasn't expected, just exit, this is
	 * not a problem. For example, if the "Implicit Return" feature is
	 * enabled, methods will always return a value.
	 *
	 * 2) If the return value can be of any type, then we cannot perform any
	 * validation, exit.
	 */
	if ((!predefined->info.expected_btypes) ||
	    (predefined->info.expected_btypes == ACPI_RTYPE_ALL)) {
		goto cleanup;
	}

	/* Create the parameter data block for object validation */

	data = ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_predefined_data));
	if (!data) {
		goto cleanup;
	}
	data->predefined = predefined;
	data->node_flags = node->flags;
	data->pathname = pathname;

	/*
	 * Check that the type of the main return object is what is expected
	 * for this predefined name
	 */
	status = acpi_ns_check_object_type(data, return_object_ptr,
					   predefined->info.expected_btypes,
					   ACPI_NOT_PACKAGE_ELEMENT);
	if (ACPI_FAILURE(status)) {
		goto exit;
	}

	/*
	 * For returned Package objects, check the type of all sub-objects.
	 * Note: Package may have been newly created by call above.
	 */
	if ((*return_object_ptr)->common.type == ACPI_TYPE_PACKAGE) {
		data->parent_package = *return_object_ptr;
		status = acpi_ns_check_package(data, return_object_ptr);
		if (ACPI_FAILURE(status)) {
			goto exit;
		}
	}

	/*
	 * The return object was OK, or it was successfully repaired above.
	 * Now make some additional checks such as verifying that package
	 * objects are sorted correctly (if required) or buffer objects have
	 * the correct data width (bytes vs. dwords). These repairs are
	 * performed on a per-name basis, i.e., the code is specific to
	 * particular predefined names.
	 */
	status = acpi_ns_complex_repairs(data, node, status, return_object_ptr);

exit:
	/*
	 * If the object validation failed or if we successfully repaired one
	 * or more objects, mark the parent node to suppress further warning
	 * messages during the next evaluation of the same method/object.
	 */
	if (ACPI_FAILURE(status) || (data->flags & ACPI_OBJECT_REPAIRED)) {
		node->flags |= ANOBJ_EVALUATED;
	}
	ACPI_FREE(data);

cleanup:
	ACPI_FREE(pathname);
	return (status);
}


void
acpi_ns_check_parameter_count(char *pathname,
			      struct acpi_namespace_node *node,
			      u32 user_param_count,
			      const union acpi_predefined_info *predefined)
{
	u32 param_count;
	u32 required_params_current;
	u32 required_params_old;

	/* Methods have 0-7 parameters. All other types have zero. */

	param_count = 0;
	if (node->type == ACPI_TYPE_METHOD) {
		param_count = node->object->method.param_count;
	}

	if (!predefined) {
		/*
		 * Check the parameter count for non-predefined methods/objects.
		 *
		 * Warning if too few or too many arguments have been passed by the
		 * caller. An incorrect number of arguments may not cause the method
		 * to fail. However, the method will fail if there are too few
		 * arguments and the method attempts to use one of the missing ones.
		 */
		if (user_param_count < param_count) {
			ACPI_WARN_PREDEFINED((AE_INFO, pathname,
					      ACPI_WARN_ALWAYS,
					      "Insufficient arguments - needs %u, found %u",
					      param_count, user_param_count));
		} else if (user_param_count > param_count) {
			ACPI_WARN_PREDEFINED((AE_INFO, pathname,
					      ACPI_WARN_ALWAYS,
					      "Excess arguments - needs %u, found %u",
					      param_count, user_param_count));
		}
		return;
	}

	/*
	 * Validate the user-supplied parameter count.
	 * Allow two different legal argument counts (_SCP, etc.)
	 */
	required_params_current = predefined->info.param_count & 0x0F;
	required_params_old = predefined->info.param_count >> 4;

	if (user_param_count != ACPI_UINT32_MAX) {
		if ((user_param_count != required_params_current) &&
		    (user_param_count != required_params_old)) {
			ACPI_WARN_PREDEFINED((AE_INFO, pathname,
					      ACPI_WARN_ALWAYS,
					      "Parameter count mismatch - "
					      "caller passed %u, ACPI requires %u",
					      user_param_count,
					      required_params_current));
		}
	}

	/*
	 * Check that the ASL-defined parameter count is what is expected for
	 * this predefined name (parameter count as defined by the ACPI
	 * specification)
	 */
	if ((param_count != required_params_current) &&
	    (param_count != required_params_old)) {
		ACPI_WARN_PREDEFINED((AE_INFO, pathname, node->flags,
				      "Parameter count mismatch - ASL declared %u, ACPI requires %u",
				      param_count, required_params_current));
	}
}


const union acpi_predefined_info *acpi_ns_check_for_predefined_name(struct
								    acpi_namespace_node
								    *node)
{
	const union acpi_predefined_info *this_name;

	/* Quick check for a predefined name, first character must be underscore */

	if (node->name.ascii[0] != '_') {
		return (NULL);
	}

	/* Search info table for a predefined method/object name */

	this_name = predefined_names;
	while (this_name->info.name[0]) {
		if (ACPI_COMPARE_NAME(node->name.ascii, this_name->info.name)) {
			return (this_name);
		}

		/*
		 * Skip next entry in the table if this name returns a Package
		 * (next entry contains the package info)
		 */
		if (this_name->info.expected_btypes & ACPI_RTYPE_PACKAGE) {
			this_name++;
		}

		this_name++;
	}

	return (NULL);		/* Not found */
}


static acpi_status
acpi_ns_check_package(struct acpi_predefined_data *data,
		      union acpi_operand_object **return_object_ptr)
{
	union acpi_operand_object *return_object = *return_object_ptr;
	const union acpi_predefined_info *package;
	union acpi_operand_object **elements;
	acpi_status status = AE_OK;
	u32 expected_count;
	u32 count;
	u32 i;

	ACPI_FUNCTION_NAME(ns_check_package);

	/* The package info for this name is in the next table entry */

	package = data->predefined + 1;

	ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
			  "%s Validating return Package of Type %X, Count %X\n",
			  data->pathname, package->ret_info.type,
			  return_object->package.count));

	/*
	 * For variable-length Packages, we can safely remove all embedded
	 * and trailing NULL package elements
	 */
	acpi_ns_remove_null_elements(data, package->ret_info.type,
				     return_object);

	/* Extract package count and elements array */

	elements = return_object->package.elements;
	count = return_object->package.count;

	/* The package must have at least one element, else invalid */

	if (!count) {
		ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
				      "Return Package has no elements (empty)"));

		return (AE_AML_OPERAND_VALUE);
	}

	/*
	 * Decode the type of the expected package contents
	 *
	 * PTYPE1 packages contain no subpackages
	 * PTYPE2 packages contain sub-packages
	 */
	switch (package->ret_info.type) {
	case ACPI_PTYPE1_FIXED:

		/*
		 * The package count is fixed and there are no sub-packages
		 *
		 * If package is too small, exit.
		 * If package is larger than expected, issue warning but continue
		 */
		expected_count =
		    package->ret_info.count1 + package->ret_info.count2;
		if (count < expected_count) {
			goto package_too_small;
		} else if (count > expected_count) {
			ACPI_DEBUG_PRINT((ACPI_DB_REPAIR,
					  "%s: Return Package is larger than needed - "
					  "found %u, expected %u\n",
					  data->pathname, count,
					  expected_count));
		}

		/* Validate all elements of the returned package */

		status = acpi_ns_check_package_elements(data, elements,
							package->ret_info.
							object_type1,
							package->ret_info.
							count1,
							package->ret_info.
							object_type2,
							package->ret_info.
							count2, 0);
		break;

	case ACPI_PTYPE1_VAR:

		/*
		 * The package count is variable, there are no sub-packages, and all
		 * elements must be of the same type
		 */
		for (i = 0; i < count; i++) {
			status = acpi_ns_check_object_type(data, elements,
							   package->ret_info.
							   object_type1, i);
			if (ACPI_FAILURE(status)) {
				return (status);
			}
			elements++;
		}
		break;

	case ACPI_PTYPE1_OPTION:

		/*
		 * The package count is variable, there are no sub-packages. There are
		 * a fixed number of required elements, and a variable number of
		 * optional elements.
		 *
		 * Check if package is at least as large as the minimum required
		 */
		expected_count = package->ret_info3.count;
		if (count < expected_count) {
			goto package_too_small;
		}

		/* Variable number of sub-objects */

		for (i = 0; i < count; i++) {
			if (i < package->ret_info3.count) {

				/* These are the required package elements (0, 1, or 2) */

				status =
				    acpi_ns_check_object_type(data, elements,
							      package->
							      ret_info3.
							      object_type[i],
							      i);
				if (ACPI_FAILURE(status)) {
					return (status);
				}
			} else {
				/* These are the optional package elements */

				status =
				    acpi_ns_check_object_type(data, elements,
							      package->
							      ret_info3.
							      tail_object_type,
							      i);
				if (ACPI_FAILURE(status)) {
					return (status);
				}
			}
			elements++;
		}
		break;

	case ACPI_PTYPE2_REV_FIXED:

		/* First element is the (Integer) revision */

		status = acpi_ns_check_object_type(data, elements,
						   ACPI_RTYPE_INTEGER, 0);
		if (ACPI_FAILURE(status)) {
			return (status);
		}

		elements++;
		count--;

		/* Examine the sub-packages */

		status =
		    acpi_ns_check_package_list(data, package, elements, count);
		break;

	case ACPI_PTYPE2_PKG_COUNT:

		/* First element is the (Integer) count of sub-packages to follow */

		status = acpi_ns_check_object_type(data, elements,
						   ACPI_RTYPE_INTEGER, 0);
		if (ACPI_FAILURE(status)) {
			return (status);
		}

		/*
		 * Count cannot be larger than the parent package length, but allow it
		 * to be smaller. The >= accounts for the Integer above.
		 */
		expected_count = (u32) (*elements)->integer.value;
		if (expected_count >= count) {
			goto package_too_small;
		}

		count = expected_count;
		elements++;

		/* Examine the sub-packages */

		status =
		    acpi_ns_check_package_list(data, package, elements, count);
		break;

	case ACPI_PTYPE2:
	case ACPI_PTYPE2_FIXED:
	case ACPI_PTYPE2_MIN:
	case ACPI_PTYPE2_COUNT:

		/*
		 * These types all return a single Package that consists of a
		 * variable number of sub-Packages.
		 *
		 * First, ensure that the first element is a sub-Package. If not,
		 * the BIOS may have incorrectly returned the object as a single
		 * package instead of a Package of Packages (a common error if
		 * there is only one entry). We may be able to repair this by
		 * wrapping the returned Package with a new outer Package.
		 */
		if (*elements
		    && ((*elements)->common.type != ACPI_TYPE_PACKAGE)) {

			/* Create the new outer package and populate it */

			status =
			    acpi_ns_repair_package_list(data,
							return_object_ptr);
			if (ACPI_FAILURE(status)) {
				return (status);
			}

			/* Update locals to point to the new package (of 1 element) */

			return_object = *return_object_ptr;
			elements = return_object->package.elements;
			count = 1;
		}

		/* Examine the sub-packages */

		status =
		    acpi_ns_check_package_list(data, package, elements, count);
		break;

	default:

		/* Should not get here if predefined info table is correct */

		ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
				      "Invalid internal return type in table entry: %X",
				      package->ret_info.type));

		return (AE_AML_INTERNAL);
	}

	return (status);

package_too_small:

	/* Error exit for the case with an incorrect package count */

	ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
			      "Return Package is too small - found %u elements, expected %u",
			      count, expected_count));

	return (AE_AML_OPERAND_VALUE);
}


static acpi_status
acpi_ns_check_package_list(struct acpi_predefined_data *data,
			   const union acpi_predefined_info *package,
			   union acpi_operand_object **elements, u32 count)
{
	union acpi_operand_object *sub_package;
	union acpi_operand_object **sub_elements;
	acpi_status status;
	u32 expected_count;
	u32 i;
	u32 j;

	/*
	 * Validate each sub-Package in the parent Package
	 *
	 * NOTE: assumes list of sub-packages contains no NULL elements.
	 * Any NULL elements should have been removed by earlier call
	 * to acpi_ns_remove_null_elements.
	 */
	for (i = 0; i < count; i++) {
		sub_package = *elements;
		sub_elements = sub_package->package.elements;
		data->parent_package = sub_package;

		/* Each sub-object must be of type Package */

		status = acpi_ns_check_object_type(data, &sub_package,
						   ACPI_RTYPE_PACKAGE, i);
		if (ACPI_FAILURE(status)) {
			return (status);
		}

		/* Examine the different types of expected sub-packages */

		data->parent_package = sub_package;
		switch (package->ret_info.type) {
		case ACPI_PTYPE2:
		case ACPI_PTYPE2_PKG_COUNT:
		case ACPI_PTYPE2_REV_FIXED:

			/* Each subpackage has a fixed number of elements */

			expected_count =
			    package->ret_info.count1 + package->ret_info.count2;
			if (sub_package->package.count < expected_count) {
				goto package_too_small;
			}

			status =
			    acpi_ns_check_package_elements(data, sub_elements,
							   package->ret_info.
							   object_type1,
							   package->ret_info.
							   count1,
							   package->ret_info.
							   object_type2,
							   package->ret_info.
							   count2, 0);
			if (ACPI_FAILURE(status)) {
				return (status);
			}
			break;

		case ACPI_PTYPE2_FIXED:

			/* Each sub-package has a fixed length */

			expected_count = package->ret_info2.count;
			if (sub_package->package.count < expected_count) {
				goto package_too_small;
			}

			/* Check the type of each sub-package element */

			for (j = 0; j < expected_count; j++) {
				status =
				    acpi_ns_check_object_type(data,
							      &sub_elements[j],
							      package->
							      ret_info2.
							      object_type[j],
							      j);
				if (ACPI_FAILURE(status)) {
					return (status);
				}
			}
			break;

		case ACPI_PTYPE2_MIN:

			/* Each sub-package has a variable but minimum length */

			expected_count = package->ret_info.count1;
			if (sub_package->package.count < expected_count) {
				goto package_too_small;
			}

			/* Check the type of each sub-package element */

			status =
			    acpi_ns_check_package_elements(data, sub_elements,
							   package->ret_info.
							   object_type1,
							   sub_package->package.
							   count, 0, 0, 0);
			if (ACPI_FAILURE(status)) {
				return (status);
			}
			break;

		case ACPI_PTYPE2_COUNT:

			/*
			 * First element is the (Integer) count of elements, including
			 * the count field (the ACPI name is num_elements)
			 */
			status = acpi_ns_check_object_type(data, sub_elements,
							   ACPI_RTYPE_INTEGER,
							   0);
			if (ACPI_FAILURE(status)) {
				return (status);
			}

			/*
			 * Make sure package is large enough for the Count and is
			 * is as large as the minimum size
			 */
			expected_count = (u32)(*sub_elements)->integer.value;
			if (sub_package->package.count < expected_count) {
				goto package_too_small;
			}
			if (sub_package->package.count <
			    package->ret_info.count1) {
				expected_count = package->ret_info.count1;
				goto package_too_small;
			}
			if (expected_count == 0) {
				/*
				 * Either the num_entries element was originally zero or it was
				 * a NULL element and repaired to an Integer of value zero.
				 * In either case, repair it by setting num_entries to be the
				 * actual size of the subpackage.
				 */
				expected_count = sub_package->package.count;
				(*sub_elements)->integer.value = expected_count;
			}

			/* Check the type of each sub-package element */

			status =
			    acpi_ns_check_package_elements(data,
							   (sub_elements + 1),
							   package->ret_info.
							   object_type1,
							   (expected_count - 1),
							   0, 0, 1);
			if (ACPI_FAILURE(status)) {
				return (status);
			}
			break;

		default:	/* Should not get here, type was validated by caller */

			return (AE_AML_INTERNAL);
		}

		elements++;
	}

	return (AE_OK);

package_too_small:

	/* The sub-package count was smaller than required */

	ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
			      "Return Sub-Package[%u] is too small - found %u elements, expected %u",
			      i, sub_package->package.count, expected_count));

	return (AE_AML_OPERAND_VALUE);
}


static acpi_status
acpi_ns_check_package_elements(struct acpi_predefined_data *data,
			       union acpi_operand_object **elements,
			       u8 type1,
			       u32 count1,
			       u8 type2, u32 count2, u32 start_index)
{
	union acpi_operand_object **this_element = elements;
	acpi_status status;
	u32 i;

	/*
	 * Up to two groups of package elements are supported by the data
	 * structure. All elements in each group must be of the same type.
	 * The second group can have a count of zero.
	 */
	for (i = 0; i < count1; i++) {
		status = acpi_ns_check_object_type(data, this_element,
						   type1, i + start_index);
		if (ACPI_FAILURE(status)) {
			return (status);
		}
		this_element++;
	}

	for (i = 0; i < count2; i++) {
		status = acpi_ns_check_object_type(data, this_element,
						   type2,
						   (i + count1 + start_index));
		if (ACPI_FAILURE(status)) {
			return (status);
		}
		this_element++;
	}

	return (AE_OK);
}


static acpi_status
acpi_ns_check_object_type(struct acpi_predefined_data *data,
			  union acpi_operand_object **return_object_ptr,
			  u32 expected_btypes, u32 package_index)
{
	union acpi_operand_object *return_object = *return_object_ptr;
	acpi_status status = AE_OK;
	u32 return_btype;
	char type_buffer[48];	/* Room for 5 types */

	/*
	 * If we get a NULL return_object here, it is a NULL package element.
	 * Since all extraneous NULL package elements were removed earlier by a
	 * call to acpi_ns_remove_null_elements, this is an unexpected NULL element.
	 * We will attempt to repair it.
	 */
	if (!return_object) {
		status = acpi_ns_repair_null_element(data, expected_btypes,
						     package_index,
						     return_object_ptr);
		if (ACPI_SUCCESS(status)) {
			return (AE_OK);	/* Repair was successful */
		}
		goto type_error_exit;
	}

	/* A Namespace node should not get here, but make sure */

	if (ACPI_GET_DESCRIPTOR_TYPE(return_object) == ACPI_DESC_TYPE_NAMED) {
		ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
				      "Invalid return type - Found a Namespace node [%4.4s] type %s",
				      return_object->node.name.ascii,
				      acpi_ut_get_type_name(return_object->node.
							    type)));
		return (AE_AML_OPERAND_TYPE);
	}

	/*
	 * Convert the object type (ACPI_TYPE_xxx) to a bitmapped object type.
	 * The bitmapped type allows multiple possible return types.
	 *
	 * Note, the cases below must handle all of the possible types returned
	 * from all of the predefined names (including elements of returned
	 * packages)
	 */
	switch (return_object->common.type) {
	case ACPI_TYPE_INTEGER:
		return_btype = ACPI_RTYPE_INTEGER;
		break;

	case ACPI_TYPE_BUFFER:
		return_btype = ACPI_RTYPE_BUFFER;
		break;

	case ACPI_TYPE_STRING:
		return_btype = ACPI_RTYPE_STRING;
		break;

	case ACPI_TYPE_PACKAGE:
		return_btype = ACPI_RTYPE_PACKAGE;
		break;

	case ACPI_TYPE_LOCAL_REFERENCE:
		return_btype = ACPI_RTYPE_REFERENCE;
		break;

	default:
		/* Not one of the supported objects, must be incorrect */

		goto type_error_exit;
	}

	/* Is the object one of the expected types? */

	if (return_btype & expected_btypes) {

		/* For reference objects, check that the reference type is correct */

		if (return_object->common.type == ACPI_TYPE_LOCAL_REFERENCE) {
			status = acpi_ns_check_reference(data, return_object);
		}

		return (status);
	}

	/* Type mismatch -- attempt repair of the returned object */

	status = acpi_ns_repair_object(data, expected_btypes,
				       package_index, return_object_ptr);
	if (ACPI_SUCCESS(status)) {
		return (AE_OK);	/* Repair was successful */
	}

      type_error_exit:

	/* Create a string with all expected types for this predefined object */

	acpi_ns_get_expected_types(type_buffer, expected_btypes);

	if (package_index == ACPI_NOT_PACKAGE_ELEMENT) {
		ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
				      "Return type mismatch - found %s, expected %s",
				      acpi_ut_get_object_type_name
				      (return_object), type_buffer));
	} else {
		ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
				      "Return Package type mismatch at index %u - "
				      "found %s, expected %s", package_index,
				      acpi_ut_get_object_type_name
				      (return_object), type_buffer));
	}

	return (AE_AML_OPERAND_TYPE);
}


static acpi_status
acpi_ns_check_reference(struct acpi_predefined_data *data,
			union acpi_operand_object *return_object)
{

	/*
	 * Check the reference object for the correct reference type (opcode).
	 * The only type of reference that can be converted to an union acpi_object is
	 * a reference to a named object (reference class: NAME)
	 */
	if (return_object->reference.class == ACPI_REFCLASS_NAME) {
		return (AE_OK);
	}

	ACPI_WARN_PREDEFINED((AE_INFO, data->pathname, data->node_flags,
			      "Return type mismatch - unexpected reference object type [%s] %2.2X",
			      acpi_ut_get_reference_name(return_object),
			      return_object->reference.class));

	return (AE_AML_OPERAND_TYPE);
}


static void acpi_ns_get_expected_types(char *buffer, u32 expected_btypes)
{
	u32 this_rtype;
	u32 i;
	u32 j;

	j = 1;
	buffer[0] = 0;
	this_rtype = ACPI_RTYPE_INTEGER;

	for (i = 0; i < ACPI_NUM_RTYPES; i++) {

		/* If one of the expected types, concatenate the name of this type */

		if (expected_btypes & this_rtype) {
			ACPI_STRCAT(buffer, &acpi_rtype_names[i][j]);
			j = 0;	/* Use name separator from now on */
		}
		this_rtype <<= 1;	/* Next Rtype */
	}
}
