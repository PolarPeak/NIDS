# -*- coding: utf-8 -*-

# from django.http import HttpResponse
from django.shortcuts import render


def index(request):
    context = {}
    context['hello'] = 'Hello World!'
    return render(request, 'baseindex.html', context)


def page_not_found(request,Exception):
    return render(request, '404a.html',{})

def page_error(request):
    return render(request, '500.html',{})
