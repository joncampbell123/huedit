#!/bin/bash
find -name \*~ -delete
find -name \*~ -exec svn del {} +
find -name \*.err -delete
