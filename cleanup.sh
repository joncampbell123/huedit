#!/bin/bash
find -name \*~ -delete
find -name \*~ -exec svn del {} +
find -name \*.err -delete
svn del hackipedia/software/huedit/huedit hackipedia/software/huedit/wcwidgen hackipedia/software/huedit/wcwidvf
find -name \*.o -exec svn del {} +

