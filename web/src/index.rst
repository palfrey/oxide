====
Home
====

.. raw:: html

   <img src="_static/images/logo.png" alt="logo" class="logo">
   <br/>

Oxide is a `desktop environment <https://en.wikipedia.org/wiki/Desktop_environment>`_ for the `reMarkable tablet <https://remarkable.com/>`_.

Features
========

- Multitasking / application switching
- Notifications
- Wifi managment
- Chroot for applications that you don't fully trust
- Optional lockscreen
- Homescreen for launching applications
- Process manager
- Take, view, and manage screenshots

Install Oxide
==============

.. raw:: html

  <div class="warning">
    ⚠️ <b>Warning:</b> Since this changes what application is launched on boot, you'll want to make sure you have your SSH password written down, and it's recommended to <a href="https://web.archive.org/web/20230616024159/https://remarkablewiki.com/tech/ssh">setup an SSH key</a>.
  </div>
  <p>
    Oxide is available in
    <a href="https://toltec-dev.org/#install-toltec">
      <img alt="toltec" src="_static/images/toltec-small.svg" class="sidebar-logo" style="width:24px;height:24px;"/>
      toltec
    </a>. These instructions assume you already have it installed.
  </p>

1. ``opkg install oxide``
2. ``systemctl disable --now xochitl``
3. If you are installing on a reMarkable 2: ``systemctl enable --now rm2fb``
4. ``systemctl enable --now tarnish``

Uninstall Oxide
===============

1. ``systemctl disable --now tarnish``
2. ``systemctl enable --now xochitl``
3. ``opkg remove -force-removal-of-dependent-packages liboxide``
