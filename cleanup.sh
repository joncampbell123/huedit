#!/bin/bash
find -name \*~ -delete
find -name \*~ -exec svn del {} +
find -name \*.err -delete
for i in hackipedia/software/huedit/huedit hackipedia/software/huedit/wcwidgen hackipedia/software/huedit/wcwidvf; do svn del $i --force; done
find -name \*.o -exec svn del {} +

