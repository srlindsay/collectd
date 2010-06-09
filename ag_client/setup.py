from distutils.core import setup
import os
import meebodev.dist

py_modules = [
		"twisted_aggregator_client"
		]

(curr_version, date, revision, description, long_description) = meebodev.dist.process_readme()

setup( version = curr_version,
	   description = description,
	   long_description = long_description,
	   name = 'twisted-aggregator-client',
	   author='meebo',
	   author_email='server@meebo.com',
	   url='http://random.meebo.com',
	   py_modules = py_modules,
	   )
