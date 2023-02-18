from django.urls import path
from . import views


urlpatterns = [
    path('', views.rule),
    path('index/', views.rule),
    path('rule/', views.rule),
    # path('monitor/', views.monitor),
    # path('savesuri/', views.savesuri),
    path('threat/', views.threat),
    path('logstat/<str:id>/', views.get_log_by_id),
    # path('delmor/<int:suid>/', views.delmor),
    path('dasboard/',views.dasboard),
    path('logs/',views.logs)



]
