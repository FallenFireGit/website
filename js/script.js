const navLinks = document.querySelectorAll('nav a');

navLinks.forEach(link => {
    link.addEventListener('click', function(e) {
        e.preventDefault();

        const targetId = this.getAttribute('href');
        const targetSection = document.querySelector(targetId);

        targetSection.scrollIntoView({ behavior: 'smooth' });
    });
});

const sections = document.querySelectorAll('section');

window.addEventListener('scroll', function() {
    let currentSection = '';

    sections.forEach(section => {
        const sectionTop = section.offsetTop -80;

        if (window.scrollY >= sectionTop) {
            currentSection = section.getAttribute('id');
        }
    });
});

navlinks.forEach(link => {
    link.classList.remove('active');

    if (link.getAttribute('href') === '#' + current) {
        link.classList.add('active');
    }
});