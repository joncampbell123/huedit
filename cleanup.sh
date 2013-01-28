#!/bin/bash
find -name \*~ -delete
find -name \*~ -exec svn del {} +
