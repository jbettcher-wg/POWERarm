/* SPDX-License-Identifier: MIT */
/* Must fail to compile: tcc's diagnostic and exit status are compared. */
int main(void)
{
	return undeclared_variable + ;
}
